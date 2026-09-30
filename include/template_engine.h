// LU6JMF - Marcelo Ferreira
// Fase 6 (tirar HTML do codigo pra templates): motor minimo de templates
// (estilo Mustache) pra permitir que as paginas dinamicas (IGATE, WX,
// SENSOR, MSG, etc - as que tem tabelas/loops gerados em C++) tambem
// possam virar arquivos em LittleFS, e nao só a casca estatica do SPA.
//
// Suporta:
//   {{var}}              - substituicao simples
//   {{a.b.c}}            - navegacao por objetos aninhados (dot path)
//   {{#if cond}}...{{/if}}
//   {{#if cond}}...{{else}}...{{/if}}
//   {{#each list}}...{{/each}}      - repete o bloco pra cada item de "list",
//                                      cada item vira o escopo mais interno
//                                      (campos do item acessados direto por
//                                      nome; pra item escalar usar "." ou
//                                      "this" pra pegar o valor do item)
//   {{#each list}}...{{else}}...{{/each}} - "else" roda se a lista estiver
//                                      vazia ou nao existir
//
// Resolucao de variavel busca do escopo mais interno (item do loop atual)
// pro mais externo (dado raiz passado pra renderTemplate), entao um campo
// de fora continua visivel de dentro de um {{#each}}.
//
// Sem escaping de HTML - mantem o mesmo padrao ja usado no resto do
// webservice.cpp (montagem de HTML por concatenacao de string, sem
// sanitizacao). So usar com dado interno/confiavel (config do proprio
// dispositivo), nunca com entrada de usuario nao tratada.
//
// Template malformado (tag sem fechamento, {{#if}}/{{#each}} sem par) nao
// trava nem gera excecao - o parser degrada de forma graciosa (encerra o
// bloco no fim do texto disponivel) pra nunca quebrar a pagina inteira por
// causa de 1 tag errada.
#ifndef TEMPLATE_ENGINE_H
#define TEMPLATE_ENGINE_H

#include <Arduino.h>
#include <ArduinoJson.h>

String renderTemplate(const String &tpl, JsonVariant data);

#endif // TEMPLATE_ENGINE_H
