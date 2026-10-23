# Test av Google-inloggning utan riktiga Google

`mock.mjs` är en låtsas-Google och en låtsas-Gmail:

- **OAuth-servern** på port 9999 har `/auth`, `/token` och `/revoke`. Den kontrollerar klient-ID och hemlighet, PKCE (S256) och redirect-URI, och har styrendpoints under `/control/` för att framkalla fel.
- **Google Drive API** på samma port (`/drive/v3/files`, `/upload/drive/v3/files` med återupptagbar uppladdning). `/control/drive` listar uppladdade filer, `/control/drive/content/<id>` hämtar innehållet och `/control/drive/fail-chunks?count=N` låter de N nästa bitarna misslyckas halvvägs, så att återupptagningen testas. En inloggning med `login_hint` som börjar med `nodrive` motsvarar att användaren avmarkerar Drive på samtyckesskärmen. Starta testinstansen med `GOOGLE_API_URL=http://brevduva-oauthmock:9999` för att testa backup till Google Drive.
- **IMAP-servrarna** kontrollerar SASL XOAUTH2 enligt Googles protokoll: port 1143 utan SASL-IR och port 1144 med. När token godkänts vidarebefordras anslutningen till GreenMail.

`apitest.mjs` kör hela flödet mot API:t: inloggning, kontoskapande, IMAP/SMTP via XOAUTH2, förnyelse, felfall, återkallelse och behörighet mellan användare.

## Köra testet

```bash
# Appen och GreenMail måste köra (Docker-nätverket heter brevduva_default)
docker compose --profile dev up -d --build

# Låtsas-Google, med kort livslängd på access-token så att förnyelsen testas
docker run -d --name brevduva-oauthmock --network brevduva_default -p 9999:9999 \
  -e ACCESS_TTL=66 -v "$PWD/tests/oauth-mock:/m" node:22-alpine node /m/mock.mjs

# En egen databas för testinstansen
docker run -d --name brevduva-idtest-db --network brevduva_default \
  -e MARIADB_DATABASE=brevduva -e MARIADB_USER=brevduva -e MARIADB_PASSWORD=testpw -e MARIADB_RANDOM_ROOT_PASSWORD=1 \
  mariadb:11.8

# En separat testinstans som pekar på låtsas-Google
docker run -d --name brevduva-idtest --network brevduva_default -p 8081:80 \
  -e DB_HOST=brevduva-idtest-db -e DB_PASSWORD=testpw \
  -e ADMIN_USERNAME=tester -e ADMIN_PASSWORD=testerpass1 \
  -e GOOGLE_CLIENT_ID=test-client -e GOOGLE_CLIENT_SECRET=test-secret -e PUBLIC_URL=http://localhost:8081 \
  -e GOOGLE_AUTH_URL=http://localhost:9999/auth \
  -e GOOGLE_TOKEN_URL=http://brevduva-oauthmock:9999/token \
  -e GOOGLE_REVOKE_URL=http://brevduva-oauthmock:9999/revoke \
  -e MAIL_CACHE=0 \
  brevduva:latest

node tests/oauth-mock/apitest.mjs

# Städa
docker rm -f brevduva-idtest brevduva-idtest-db brevduva-oauthmock
```

`GOOGLE_AUTH_URL` pekar på `localhost`, eftersom den adressen öppnas av webbläsaren (här av testskriptet). Token- och revoke-adresserna anropas av backenden inifrån Docker-nätverket.

`MAIL_CACHE=0` behövs eftersom testet förutsätter att mapplistningen går mot IMAP varje gång (med cachen på svarar servern ur cachen även när token avvisats). GreenMail måste köra med `-Dgreenmail.verbose` (som i dev-profilen) för att SMTP-kontrollen av XOAUTH2 ska hitta `AUTH`-raden i loggen.

Testinstansen och dess databas har ingen volym. Starta nya containrar för varje körning, eftersom testet förutsätter att kontona får id 1 och uppåt.
