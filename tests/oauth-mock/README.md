# Test av Google-inloggning utan riktiga Google

`mock.mjs` är en låtsas-Google och en låtsas-Gmail:

- **OAuth-servern** på port 9999 har `/auth`, `/token` och `/revoke`. Den kontrollerar klient-ID och hemlighet, PKCE (S256) och redirect-URI, och har styrendpoints under `/control/` för att framkalla fel.
- **IMAP-servrarna** kontrollerar SASL XOAUTH2 enligt Googles protokoll: port 1143 utan SASL-IR och port 1144 med. När token godkänts vidarebefordras anslutningen till GreenMail.

`apitest.mjs` kör hela flödet mot API:t: inloggning, kontoskapande, IMAP/SMTP via XOAUTH2, förnyelse, felfall, återkallelse och behörighet mellan användare.

## Köra testet

```bash
# Appen och GreenMail måste köra (nätverket heter <projektmapp>_default)
docker compose --profile dev up -d --build

# Låtsas-Google, med kort livslängd på access-token så att förnyelsen testas
docker run -d --name tbk-oauthmock --network thunderbirdklon_default -p 9999:9999 \
  -e ACCESS_TTL=66 -v "$PWD/tests/oauth-mock:/m" node:22-alpine node /m/mock.mjs

# En separat testinstans som pekar på låtsas-Google
docker run -d --name tbk-idtest --network thunderbirdklon_default -p 8081:8080 \
  -e ADMIN_USERNAME=tester -e ADMIN_PASSWORD=testerpass1 \
  -e GOOGLE_CLIENT_ID=test-client -e GOOGLE_CLIENT_SECRET=test-secret -e PUBLIC_URL=http://localhost:8081 \
  -e GOOGLE_AUTH_URL=http://localhost:9999/auth \
  -e GOOGLE_TOKEN_URL=http://tbk-oauthmock:9999/token \
  -e GOOGLE_REVOKE_URL=http://tbk-oauthmock:9999/revoke \
  thunderbirdklon:latest

node tests/oauth-mock/apitest.mjs

# Städa
docker rm -f tbk-idtest tbk-oauthmock
```

`GOOGLE_AUTH_URL` pekar på `localhost`, eftersom den adressen öppnas av webbläsaren (här av testskriptet). Token- och revoke-adresserna anropas av backenden inifrån Docker-nätverket.

Testinstansen har ingen volym. Starta en ny instans för varje körning, eftersom testet förutsätter att kontona får id 1 och uppåt.
