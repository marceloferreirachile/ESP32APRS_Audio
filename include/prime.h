#ifndef PRIME_H
#define PRIME_H

#include <Arduino.h>

// PRIME authentication token management
bool primeSaveToken(const String &token);
String primeLoadToken();
bool primeHasToken();
bool primeClearToken();

// PRIME API
bool primeLogin(const String &email, const String &password);

// LU6JMF - Marcelo Ferreira:
// PRIME account creation and password recovery.
bool primeRegister(const String &callsign, const String &email, const String &password);
bool primeRecover(const String &email);

bool primeValidate();
bool primeFeature(const String &feature);

String primeUserEmail();
String primeUserCallsign();
String primeUserPlan();

#endif
