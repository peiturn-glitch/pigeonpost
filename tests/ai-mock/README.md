# Test av AI-sortering utan en riktig AI-tjänst

`mock.mjs` är en låtsas-AI med OpenAI-kompatibelt API (`/v1/models` och `/v1/chat/completions`) på port 9998. Den sorterar efter enkla regler i ämnesraden, och modellnamnet styr beteendet:

| Modell | Beteende |
|--------|----------|
| `mock-sorter` | Svarar med ren JSON. Följer en rättning (`corrections`) för samma avsändare, och tolkar sökfrågor |
| `mock-fenced` | Avvisar `response_format` och lägger text och kodstaket runt svaret |
| `mock-evil` | Svarar med påhittade mappar, fel typer och okända meddelanden |
| `mock-flaky` | Första anropet ger 503, nästa fungerar |
| `mock-busy` | Svarar alltid 429 |
| `mock-prose` | Svarar med text utan JSON |

`mock-sorter` svarar också på regelförslag (underlaget har `inboxSample`): en giltig regel, en utan träffar och några ogiltiga som servern ska sålla bort.

`/control/requests` visar vad appen har skickat, så att testet kan kontrollera att bilagor och mer än 500 tecken text aldrig lämnar servern.

`apitest.mjs` kör hela flödet mot API:t: inställningar och nyckelhantering, listan över tillåtna tjänster, förslag för text- och HTML-mejl, val av målmappar och att valet sparas, sortering av en stor mapp i omgångar (`beforeUid` och det sparade märket), sökning i alla mappar med filter och AI-tolkning av en fråga, sortering av en hel mapp i bakgrunden (fel från AI-tjänsten, paus, ångra en flytt och ångra allt), inlärning (rättningar som exempel till AI:n, regelförslag och avsändarregler), besvärliga svar från AI-tjänsten, flytt, automatisk sortering med historik och ångra, och behörighet mellan användare.

## Köra testet

```bash
docker build -t brevduva:latest .
docker network create brevduva-aitest

docker run -d --name brevduva-aitest-db --network brevduva-aitest \
  -e MARIADB_DATABASE=brevduva -e MARIADB_USER=brevduva -e MARIADB_PASSWORD=testpw -e MARIADB_RANDOM_ROOT_PASSWORD=1 \
  mariadb:11.8

docker run -d --name brevduva-aitest-mail --network brevduva-aitest -p 127.0.0.1:13025:3025 -p 127.0.0.1:13143:3143 \
  -e GREENMAIL_OPTS="-Dgreenmail.setup.test.all -Dgreenmail.hostname=0.0.0.0 -Dgreenmail.auth.disabled" \
  greenmail/standalone:2.1.3

docker run -d --name brevduva-aimock --network brevduva-aitest -p 127.0.0.1:9998:9998 \
  -v "$PWD/tests/ai-mock:/m:ro" node:22-alpine node /m/mock.mjs

# Låtsas-FCM för push-notiserna (tests/push-mock)
docker run -d --name brevduva-pushmock --network brevduva-aitest -p 127.0.0.1:9997:9997 \
  -v "$PWD/tests/push-mock:/m:ro" node:22-alpine node /m/mock.mjs

docker run -d --name brevduva-aitest --network brevduva-aitest -p 127.0.0.1:8082:80 \
  -e DB_HOST=brevduva-aitest-db -e DB_PASSWORD=testpw \
  -e ADMIN_USERNAME=tester -e ADMIN_PASSWORD=testerpass1 \
  -e "AI_EXTRA_PROVIDERS=Testtjänst=http://brevduva-aimock:9998/v1" \
  -v "$PWD/tests/push-mock:/m/push:ro" -e FCM_SERVICE_ACCOUNT_FILE=/m/push/service-account.json \
  -e FIREBASE_CONFIG_FILE=/m/push/google-services.json \
  -e "FCM_URL=http://brevduva-pushmock:9997/v1/projects/{project}/messages:send" \
  -e FCM_TOKEN_URL=http://brevduva-pushmock:9997/token \
  -e TRASH_DAY_SECONDS=2 -e TRASH_PURGE_INTERVAL=2 -e RULE_CHECK_SECONDS=2 \
  brevduva:latest

node tests/mail/apitest.mjs     # formaterade meddelanden och flytt av mappar
node tests/ai-mock/apitest.mjs
node tests/cache/apitest.mjs    # mejlcachen: synk, sökning, ändringar och läsning när mejlservern är bortkopplad
node tests/changes/apitest.mjs  # ändringslistan för appen (/changes), sessionsförnyelse och gzip
node tests/push-mock/apitest.mjs  # push-notiser: enheter, utskick till låtsas-FCM, mappval, dubbletter, fel
node tests/trash/apitest.mjs      # tömning av papperskorgen efter ett antal "dagar" (2 s med variablerna ovan)
node tests/rules/apitest.mjs      # regler för inkommande post, körning på ny post och hela mappar, förslag med och utan låtsas-AI:n

# Städa
docker rm -f brevduva-aitest brevduva-aitest-db brevduva-aitest-mail brevduva-aimock brevduva-pushmock
docker network rm brevduva-aitest
```

Testet förutsätter en tom databas och en tom brevlåda, så ta bort containrarna mellan körningarna.

Produktionen kör MariaDB 10.3.9. Vid en schemaändring: kör stacken en gång med `mariadb:10.3.9` (den förstår bara `MYSQL_DATABASE`/`MYSQL_USER`/`MYSQL_PASSWORD`/`MYSQL_RANDOM_ROOT_PASSWORD`), starta appen först på den gamla avbilden och byt sedan till den nya på samma databas, så att migreringen testas som produktionen ser den.
