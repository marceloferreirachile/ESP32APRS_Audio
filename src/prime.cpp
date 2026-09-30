#include "prime.h"

#include <LITTLEFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char *PRIME_TOKEN_FILE = "/prime_token.dat";
static const char *PRIME_API_URL = "https://prime-api.tracktotal.ar/api";

static String gPrimeEmail;
static String gPrimeCallsign;
static String gPrimePlan;

// v-lu6jmf: primeValidate()/primeFeature() were being called synchronously on
// EVERY load of /prime and /txdebug (webservice.cpp), each one a blocking
// HTTPS/TLS round-trip to an external server with an 8s timeout. Opening
// /txdebug did this TWICE in a row (validate + feature check) - up to 16s of
// the web server blocked on one request, worse if the internet link is slow
// or down (very plausible for a field radio on a flaky connection), which
// reads to the user as the whole device "hanging". Caching the result for a
// few minutes means normal page reloads/navigation don't re-hit the network
// at all, while still re-checking periodically so a revoked/expired account
// is caught reasonably fast. Login/logout still force a fresh check.
static const unsigned long PRIME_VALIDATE_OK_TTL_MS = 5UL * 60UL * 1000UL;  // re-check a valid session every 5 min
static const unsigned long PRIME_VALIDATE_FAIL_TTL_MS = 20UL * 1000UL;      // but don't retry-storm the API for 20s after any failure (network down, 401, etc.)
static bool gPrimeValidCached = false;
static unsigned long gPrimeValidatedAt = 0; // millis() of the last check, either outcome; 0 = never checked yet
static String gPrimeFeatureCachedName;
static bool gPrimeFeatureCachedResult = false;
static unsigned long gPrimeFeatureCachedAt = 0;

static String urlEncode(const String &value)
{
    String encoded;

    for (size_t i = 0; i < value.length(); i++)
    {
        char c = value[i];

        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else
        {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded += hex;
        }
    }

    return encoded;
}

// -----------------------------------------------------------------------------
// Local token storage
// -----------------------------------------------------------------------------

bool primeSaveToken(const String &token)
{
    if (token.length() == 0)
        return false;

    File file = LITTLEFS.open(PRIME_TOKEN_FILE, "w");

    if (!file)
        return false;

    size_t written = file.print(token);
    file.close();

    return written == token.length();
}

String primeLoadToken()
{
    if (!LITTLEFS.exists(PRIME_TOKEN_FILE))
        return "";

    File file = LITTLEFS.open(PRIME_TOKEN_FILE, "r");

    if (!file)
        return "";

    String token = file.readString();
    file.close();

    token.trim();

    return token;
}

bool primeHasToken()
{
    return primeLoadToken().length() > 0;
}

bool primeClearToken()
{
    gPrimeEmail = "";
    gPrimeCallsign = "";
    gPrimePlan = "";

    // v-lu6jmf: invalidate the cache too, otherwise a just-logged-out session
    // would keep reading as valid until the TTL naturally expired.
    gPrimeValidCached = false;
    gPrimeValidatedAt = 0;
    gPrimeFeatureCachedName = "";
    gPrimeFeatureCachedAt = 0;

    if (!LITTLEFS.exists(PRIME_TOKEN_FILE))
        return true;

    return LITTLEFS.remove(PRIME_TOKEN_FILE);
}

// -----------------------------------------------------------------------------
// PRIME LOGIN
// -----------------------------------------------------------------------------

bool primeLogin(const String &email, const String &password)
{
    if (email.length() == 0 || password.length() == 0)
        return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = String(PRIME_API_URL) + "/login.php";

    if (!http.begin(client, url))
        return false;

    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body;
    body.reserve(email.length() + password.length() + 20);

    body += "email=";
    body += urlEncode(email);
    body += "&password=";
    body += urlEncode(password);

    int httpCode = http.POST(body);

    if (httpCode <= 0)
    {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (httpCode != HTTP_CODE_OK)
        return false;

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, payload);

    if (error)
        return false;

    bool success = doc["success"] | false;

    if (!success)
        return false;

    const char *token = doc["token"] | "";
    const char *indicativo = doc["user"]["indicativo"] | "";
    const char *userEmail = doc["user"]["email"] | "";
    const char *plan = doc["user"]["plan"] | "";

    if (token[0] == '\0')
        return false;

    if (!primeSaveToken(String(token)))
        return false;

    gPrimeEmail = String(userEmail);
    gPrimeCallsign = String(indicativo);
    gPrimePlan = String(plan);

    // v-lu6jmf: seed the validate cache right away - login already just proved
    // the token is good, so the page that follows the login (almost always
    // /prime) doesn't need to immediately pay for a second blocking HTTPS call.
    gPrimeValidCached = true;
    gPrimeValidatedAt = millis();

    return true;
}

// -----------------------------------------------------------------------------
// LU6JMF - Marcelo Ferreira:
// PRIME account registration.
//
// Remote contract:
// POST /api/register.php
//   indicativo
//   email
//   password
// -----------------------------------------------------------------------------

bool primeRegister(const String &callsign, const String &email, const String &password)
{
    if (callsign.length() == 0 ||
        email.length() == 0 ||
        password.length() < 8)
    {
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = String(PRIME_API_URL) + "/register.php";

    if (!http.begin(client, url))
        return false;

    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body;
    body.reserve(callsign.length() + email.length() + password.length() + 50);

    body += "indicativo=";
    body += urlEncode(callsign);
    body += "&email=";
    body += urlEncode(email);
    body += "&password=";
    body += urlEncode(password);

    int httpCode = http.POST(body);

    if (httpCode <= 0)
    {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (httpCode != HTTP_CODE_OK &&
        httpCode != 201)
    {
        return false;
    }

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, payload);

    if (error)
        return false;

    return doc["success"] | false;
}

// -----------------------------------------------------------------------------
// LU6JMF - Marcelo Ferreira:
// PRIME password recovery request.
//
// The API intentionally returns the same public response regardless
// of whether the e-mail address exists.
// -----------------------------------------------------------------------------

bool primeRecover(const String &email)
{
    if (email.length() == 0)
        return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = String(PRIME_API_URL) + "/recover.php";

    if (!http.begin(client, url))
        return false;

    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body;
    body.reserve(email.length() + 20);

    body += "email=";
    body += urlEncode(email);

    int httpCode = http.POST(body);

    if (httpCode <= 0)
    {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (httpCode != HTTP_CODE_OK)
        return false;

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, payload);

    if (error)
        return false;

    return doc["success"] | false;
}

// -----------------------------------------------------------------------------
// Temporariamente aguardando os contratos já existentes de me.php/feature.php.
// Não inventamos esses endpoints.
// -----------------------------------------------------------------------------

// LU6JMF - Marcelo Ferreira
// Validate the persisted PRIME token against the PRIME API.
bool primeValidate()
{
    String token = primeLoadToken();

    if (token.length() == 0)
        return false;

    // v-lu6jmf: serve from cache when still fresh - see the TTL comment above.
    // A valid session skips the blocking HTTPS round-trip for 5 min; a failed
    // check (network down, 401, etc.) is still remembered for 20s so a burst
    // of page loads/reloads while the link is bad doesn't each block for up
    // to 8s in a row.
    if (gPrimeValidatedAt != 0)
    {
        unsigned long age = millis() - gPrimeValidatedAt;
        if (gPrimeValidCached && age < PRIME_VALIDATE_OK_TTL_MS)
            return true;
        if (!gPrimeValidCached && age < PRIME_VALIDATE_FAIL_TTL_MS)
            return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = String(PRIME_API_URL) + "/me.php";

    if (!http.begin(client, url))
    {
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    http.setTimeout(8000);
    http.addHeader("Authorization", "Bearer " + token);

    int httpCode = http.GET();

    if (httpCode <= 0)
    {
        http.end();
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (httpCode == 401)
    {
        primeClearToken(); // also resets the cache - see primeClearToken()
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    if (httpCode != HTTP_CODE_OK)
    {
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, payload);

    if (error)
    {
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    if (!(doc["success"] | false))
    {
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    const char *indicativo = doc["user"]["indicativo"] | "";
    const char *email = doc["user"]["email"] | "";
    const char *plan = doc["user"]["plan"] | "";
    const char *status = doc["user"]["status"] | "";

    if (status[0] != '\0' && strcmp(status, "active") != 0)
    {
        gPrimeValidCached = false;
        gPrimeValidatedAt = millis();
        return false;
    }

    gPrimeValidCached = true;
    gPrimeValidatedAt = millis();

    gPrimeCallsign = String(indicativo);
    gPrimeEmail = String(email);
    gPrimePlan = String(plan);

    return true;
}

// LU6JMF - Marcelo Ferreira
// Check PRIME feature access using the persisted authentication token.
bool primeFeature(const String &feature)
{
    String token = primeLoadToken();

    if (token.length() == 0 || feature.length() == 0)
        return false;

    // v-lu6jmf: same caching rationale as primeValidate() above - handle_prime()
    // and handle_txdebug() call this right after primeValidate() on every page
    // load, doubling the blocking-HTTPS exposure. Depth-1 cache (last feature
    // name checked) is enough since "txdebug" is the only feature checked today.
    if (gPrimeFeatureCachedName == feature && gPrimeFeatureCachedAt != 0)
    {
        unsigned long age = millis() - gPrimeFeatureCachedAt;
        if (gPrimeFeatureCachedResult && age < PRIME_VALIDATE_OK_TTL_MS)
            return true;
        if (!gPrimeFeatureCachedResult && age < PRIME_VALIDATE_FAIL_TTL_MS)
            return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = String(PRIME_API_URL) + "/feature.php?feature=" + urlEncode(feature);

    if (!http.begin(client, url))
    {
        gPrimeFeatureCachedName = feature;
        gPrimeFeatureCachedResult = false;
        gPrimeFeatureCachedAt = millis();
        return false;
    }

    http.setTimeout(8000);
    http.addHeader("Authorization", "Bearer " + token);

    int httpCode = http.GET();

    if (httpCode <= 0)
    {
        http.end();
        gPrimeFeatureCachedName = feature;
        gPrimeFeatureCachedResult = false;
        gPrimeFeatureCachedAt = millis();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (httpCode == 401)
    {
        primeClearToken();
        gPrimeFeatureCachedName = feature;
        gPrimeFeatureCachedResult = false;
        gPrimeFeatureCachedAt = millis();
        return false;
    }

    if (httpCode != HTTP_CODE_OK)
    {
        gPrimeFeatureCachedName = feature;
        gPrimeFeatureCachedResult = false;
        gPrimeFeatureCachedAt = millis();
        return false;
    }

    JsonDocument doc;

    DeserializationError error = deserializeJson(doc, payload);

    if (error)
    {
        gPrimeFeatureCachedName = feature;
        gPrimeFeatureCachedResult = false;
        gPrimeFeatureCachedAt = millis();
        return false;
    }

    if (!(doc["success"] | false))
    {
        gPrimeFeatureCachedName = feature;
        gPrimeFeatureCachedResult = false;
        gPrimeFeatureCachedAt = millis();
        return false;
    }

    const char *plan = doc["plan"] | "";
    const char *indicativo = doc["indicativo"] | "";
    bool allowed = doc["allowed"] | false;

    if (plan[0] != '\0')
        gPrimePlan = String(plan);

    if (indicativo[0] != '\0')
        gPrimeCallsign = String(indicativo);

    gPrimeFeatureCachedName = feature;
    gPrimeFeatureCachedResult = allowed;
    gPrimeFeatureCachedAt = millis();

    return allowed;
}

// -----------------------------------------------------------------------------
// Current PRIME user
// -----------------------------------------------------------------------------

String primeUserEmail()
{
    return gPrimeEmail;
}

String primeUserCallsign()
{
    return gPrimeCallsign;
}

String primeUserPlan()
{
    return gPrimePlan;
}
