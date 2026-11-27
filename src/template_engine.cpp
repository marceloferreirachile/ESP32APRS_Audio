// LU6JMF - Marcelo Ferreira
// Implementacao do motor de templates - ver include/template_engine.h pra
// documentacao da sintaxe suportada e das decisoes de design (sem escaping,
// degrada gracioso em template malformado).
#include "template_engine.h"
#include <vector>

namespace
{
	// "." ou "this" significa "o valor do escopo mais interno, inteiro" -
	// usado dentro de um {{#each}} sobre uma lista de valores escalares
	// (em vez de uma lista de objetos com campos nomeados).
	bool isSelfPath(const String &path)
	{
		return path == "." || path == "this";
	}

	// Busca "path" (com notacao de ponto, ex: "config.rf2inet") nos escopos,
	// do mais interno (fim do vetor) pro mais externo (inicio) - assim um
	// campo de fora do {{#each}} continua visivel de dentro do loop.
	JsonVariant resolvePath(const String &path, const std::vector<JsonVariant> &scopes)
	{
		if (isSelfPath(path))
		{
			if (!scopes.empty())
				return scopes.back();
			return JsonVariant();
		}

		for (int s = (int)scopes.size() - 1; s >= 0; s--)
		{
			JsonVariant cur = scopes[s];
			bool ok = true;
			int start = 0;
			while (start <= (int)path.length())
			{
				int dot = path.indexOf('.', start);
				String seg = (dot == -1) ? path.substring(start) : path.substring(start, dot);
				if (cur.isNull() || !cur.is<JsonObject>() || !cur.as<JsonObject>().containsKey(seg))
				{
					ok = false;
					break;
				}
				cur = cur[seg];
				if (dot == -1)
					break;
				start = dot + 1;
			}
			if (ok)
				return cur;
		}
		return JsonVariant(); // nao encontrado em nenhum escopo - trata como vazio/falso
	}

	bool isTruthy(JsonVariant v)
	{
		if (v.isNull())
			return false;
		if (v.is<bool>())
			return v.as<bool>();
		if (v.is<JsonArray>())
			return v.as<JsonArray>().size() > 0;
		if (v.is<JsonObject>())
			return v.as<JsonObject>().size() > 0;
		if (v.is<const char *>())
		{
			const char *s = v.as<const char *>();
			return s != nullptr && s[0] != '\0';
		}
		if (v.is<float>() || v.is<double>() || v.is<int>() || v.is<long>() || v.is<unsigned int>())
			return v.as<double>() != 0.0;
		return true;
	}

	String variantToString(JsonVariant v)
	{
		if (v.isNull())
			return String("");
		if (v.is<const char *>())
			return String(v.as<const char *>());
		if (v.is<bool>())
			return v.as<bool>() ? String("true") : String("false");
		if (v.is<JsonObject>() || v.is<JsonArray>())
			return String(""); // nao tem sentido imprimir um objeto/array cru na pagina
		return v.as<String>();
	}

	// 2.1.9: the engine works directly on the template text (const char*) and writes every part of
	// the page into ONE output String. Before, a page template kept in flash was first copied into a
	// String (a 14 KB copy for System) and every {{#if}}/{{#each}} block was built in its own String and
	// then appended: with a fragmented heap one of those allocations could fail and that part of the
	// page silently disappeared (seen: "Display Setting" missing from the System page). Same output.

	// position of pat in tpl[from, to) or -1
	int findIn(const char *tpl, int from, int to, const char *pat)
	{
		size_t pl = strlen(pat);
		for (int k = from; k + (int)pl <= to; k++)
			if (tpl[k] == pat[0] && memcmp(tpl + k, pat, pl) == 0)
				return k;
		return -1;
	}

	String tagText(const char *tpl, int a, int b)
	{
		String t;
		t.concat(tpl + a, (unsigned int)(b - a));
		t.trim();
		return t;
	}

	// Varre a partir de "from" (logo depois do "}}" da tag de abertura) e acha
	// a extensao do bloco: onde o corpo termina (antes do {{/if}} ou
	// {{/each}} correspondente, respeitando blocos aninhados), onde comeca e
	// termina um {{else}} no MESMO nivel (se houver), e onde o texto
	// continua depois da tag de fechamento. Se o template estiver malformado
	// (tag sem fechamento em algum lugar), degrada fechando o bloco no fim
	// do texto disponivel em vez de travar.
	void findBlockExtent(const char *tpl, int len, int from, int to,
						  int &blockBodyEnd, int &elseTagStart, int &elseTagEnd, int &afterCloseTag)
	{
		elseTagStart = -1;
		elseTagEnd = -1;
		int depth = 0;
		int i = from;
		while (i < to)
		{
			int tagStart = findIn(tpl, i, len, "{{");
			if (tagStart == -1 || tagStart >= to)
				break;
			int tagEnd = findIn(tpl, tagStart + 2, len, "}}");
			if (tagEnd == -1 || tagEnd >= to)
				break;

			String tagContent = tagText(tpl, tagStart + 2, tagEnd);

			if (tagContent.startsWith("#if ") || tagContent.startsWith("#each "))
			{
				depth++;
			}
			else if (tagContent == "/if" || tagContent == "/each")
			{
				if (depth == 0)
				{
					blockBodyEnd = tagStart;
					afterCloseTag = tagEnd + 2;
					return;
				}
				depth--;
			}
			else if (tagContent == "else" && depth == 0)
			{
				elseTagStart = tagStart;
				elseTagEnd = tagEnd + 2;
			}
			i = tagEnd + 2;
		}
		// Malformado/sem fechamento - encerra no fim do texto disponivel em
		// vez de travar ou entrar em loop infinito.
		blockBodyEnd = to;
		afterCloseTag = to;
	}

	void renderBlock(String &out, const char *tpl, int len, int from, int to, std::vector<JsonVariant> &scopes)
	{
		int i = from;
		while (i < to)
		{
			int tagStart = findIn(tpl, i, len, "{{");
			if (tagStart == -1 || tagStart >= to)
			{
				out.concat(tpl + i, (unsigned int)(to - i));
				break;
			}
			out.concat(tpl + i, (unsigned int)(tagStart - i));

			int tagEnd = findIn(tpl, tagStart + 2, len, "}}");
			if (tagEnd == -1 || tagEnd >= to)
			{
				// tag sem fechamento - emite o resto literal e para, em vez
				// de travar.
				out.concat(tpl + tagStart, (unsigned int)(to - tagStart));
				break;
			}

			String tagContent = tagText(tpl, tagStart + 2, tagEnd);

			if (tagContent.startsWith("#if "))
			{
				String cond = tagContent.substring(4);
				cond.trim();

				int blockBodyEnd, elseTagStart, elseTagEnd, afterCloseTag;
				findBlockExtent(tpl, len, tagEnd + 2, to, blockBodyEnd, elseTagStart, elseTagEnd, afterCloseTag);

				bool truthy = isTruthy(resolvePath(cond, scopes));
				if (truthy)
				{
					int thenEnd = (elseTagStart != -1) ? elseTagStart : blockBodyEnd;
					renderBlock(out, tpl, len, tagEnd + 2, thenEnd, scopes);
				}
				else if (elseTagStart != -1)
				{
					renderBlock(out, tpl, len, elseTagEnd, blockBodyEnd, scopes);
				}
				i = afterCloseTag;
			}
			else if (tagContent.startsWith("#each "))
			{
				String arrPath = tagContent.substring(6);
				arrPath.trim();

				int blockBodyEnd, elseTagStart, elseTagEnd, afterCloseTag;
				findBlockExtent(tpl, len, tagEnd + 2, to, blockBodyEnd, elseTagStart, elseTagEnd, afterCloseTag);

				JsonVariant arr = resolvePath(arrPath, scopes);
				int bodyEnd = (elseTagStart != -1) ? elseTagStart : blockBodyEnd;
				if (arr.is<JsonArray>() && arr.as<JsonArray>().size() > 0)
				{
					for (JsonVariant item : arr.as<JsonArray>())
					{
						scopes.push_back(item);
						renderBlock(out, tpl, len, tagEnd + 2, bodyEnd, scopes);
						scopes.pop_back();
					}
				}
				else if (elseTagStart != -1)
				{
					renderBlock(out, tpl, len, elseTagEnd, blockBodyEnd, scopes);
				}
				i = afterCloseTag;
			}
			else if (tagContent == "/if" || tagContent == "/each" || tagContent == "else")
			{
				// Fecho/else orfao nesse nivel (template malformado) - ignora
				// em vez de travar, o resto do template continua rendendo.
				i = tagEnd + 2;
			}
			else
			{
				out += variantToString(resolvePath(tagContent, scopes));
				i = tagEnd + 2;
			}
		}
	}
} // namespace

String renderTemplate(const char *tpl, JsonVariant data)
{
	std::vector<JsonVariant> scopes;
	scopes.push_back(data);
	int len = (int)strlen(tpl);
	String out;
	out.reserve(len + len / 4); // the page is about the template size plus the values
	renderBlock(out, tpl, len, 0, len, scopes);
	return out;
}

String renderTemplate(const String &tpl, JsonVariant data)
{
	return renderTemplate(tpl.c_str(), data);
}
