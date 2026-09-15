# ThunderbirdKlon

En webbaserad e-postklient i Thunderbirds anda. Backenden är skriven i C++20 och frontenden i Vue 3. Allt körs i en enda Docker-container.

Klienten ansluter till riktiga e-postkonton via IMAP och SMTP, till exempel Gmail, iCloud eller en egen server.

## Funktioner

- Flera användare, där var och en har sina egna e-postkonton. En administratör skapar användare och kan återställa lösenord
- Flera e-postkonton per användare, med mappträd och antal olästa per mapp
- Gmail-konton kan logga in med Google (OAuth 2.0), så inget app-lösenord behövs (se [Google-inloggning för Gmail](#google-inloggning-för-gmail))
- Flera avsändare per konto (Thunderbirds "identiteter"), var och en med eget namn, egen adress, svarsadress och signatur. En avsändare kan använda kontots utgående server eller en egen SMTP-server med egen inloggning. Du väljer avsändare i Från-fältet, och vid svar väljs automatiskt den adress som mejlet skickades till
- Tre paneler: mappar, meddelandelista och läsruta, med storlek som går att dra om
- Läsa text- och HTML-meddelanden. HTML visas i en sandlådad iframe utan skript, och fjärrinnehåll (spårningspixlar och liknande) blockeras tills du tillåter det
- Inbäddade bilder (`cid:`) och nedladdning av bilagor
- Skriva, svara, svara alla och vidarebefordra (med originalets bilagor), med bilagor via knapp eller dra-och-släpp
- Utkast: spara, öppna och redigera, skicka
- Kopia sparas i Skickat, och besvarade meddelanden märks
- Stjärnmärka, markera som läst eller oläst, arkivera, flytta till skräppost, ta bort (till papperskorgen, eller permanent när du redan är i papperskorgen)
- Flytta meddelanden genom att dra dem till en mapp
- Import och export av konton, avsändare, servrar och signaturer i Thunderbirds format (`prefs.js`), se nedan
- Snabbfilter: fritext (ämne, från, till), olästa, stjärnmärkta. Sökningen körs på servern
- Skapa, byta namn på och ta bort mappar (högerklick)
- Hämtar nya meddelanden automatiskt varje minut
- Mörkt tema om operativsystemet använder det
- Kortkommandon: `Ctrl+N` skriv, `Ctrl+R` svara, `Ctrl+Shift+R` svara alla, `Ctrl+L` vidarebefordra, `Delete` ta bort, `↑/↓` bläddra, `S` stjärna, `M` läst/oläst, `A` arkivera, `J` skräppost, `F5` hämta. I skrivfönstret: `Ctrl+Enter` skicka, `Ctrl+S` spara utkast

## Kom igång

```bash
# Första administratören och en krypteringsnyckel för sparade e-postlösenord
export ADMIN_USERNAME='admin'
export ADMIN_PASSWORD='ett-starkt-lösenord'
export SECRET_KEY="$(openssl rand -hex 32)"

docker compose up -d --build
```

Öppna sedan http://localhost:8080 och logga in som administratör. Lägg till användare under **Meny → Hantera användare**. Varje användare lägger sedan till sina egna e-postkonton, och kontoguiden fyller i serverinställningar för Gmail, Outlook, iCloud och Yahoo utifrån e-postadressen.

Utan `ADMIN_USERNAME` och `ADMIN_PASSWORD` visas en installationssida första gången, och den som först öppnar appen blir administratör. Gör det innan tjänsten nås av andra. Variablerna används bara så länge databasen saknar användare, och du kan ta bort dem efteråt.

På Windows (PowerShell) sätter du variablerna med `$env:ADMIN_PASSWORD = '...'`. Du kan också lägga dem i en `.env`-fil bredvid `docker-compose.yml`.

### Uppgradera från enanvändarversionen

`APP_PASSWORD` används inte längre. Vid första start efter uppgraderingen skapar du en administratör, antingen på installationssidan eller med variablerna ovan. E-postkonton som redan finns tillfaller den administratören.

### Testa utan riktigt konto

Profilen `dev` startar även en lokal testmailserver (GreenMail) som accepterar alla användarnamn och lösenord:

```bash
docker compose --profile dev up -d --build
```

Lägg till ett konto med förinställningen **Testserver**, till exempel adressen `test@localhost` med lösenordet `test`. Skicka ett meddelande till `test@localhost`, så hamnar det i inkorgen. GreenMail har bara en inkorg från början. Skapa mapparna `Sent`, `Drafts`, `Trash` och `Arkiv` via högerklick på kontot för att prova hela flödet.

## Flytta inställningar till och från Thunderbird

Under **Meny → Importera/exportera (Thunderbird)**:

- **Exportera** laddar ned en zip-fil med `prefs.js` i roten, samma upplägg som Thunderbirds egen profilexport. I Thunderbird väljer du **Verktyg → Importera → Thunderbird**, pekar på zip-filen och bockar i konton och inställningar. Filen innehåller IMAP-konton, extra avsändare (identiteter), utgående servrar, svarsadresser och signaturer.
- **Importera** tar emot en export från Thunderbird (**Verktyg → Exportera**) eller `prefs.js` direkt från profilmappen. Zip-filen läses i webbläsaren, även stora ZIP64-arkiv med all e-post, och bara `prefs.js` skickas till servern. Du ser en förhandsgranskning, väljer konton, anger lösenord och kan testa anslutningarna.

Så översätts inställningarna:

| Thunderbird | ThunderbirdKlon |
|-------------|-----------------|
| `mail.server.*` av typen `imap` (`hostname`/`realhostname`, `port`, `socketType`, `userName`) | E-postkonto (IMAP) |
| Kontots första identitet (`fullName`, `useremail`, `htmlSigText`) och dess `smtpServer` | Kontots namn, adress, signatur och utgående server |
| Övriga identiteter | Extra avsändare. Samma SMTP-server som kontot ger "Använd kontots server", annars egen SMTP-server |
| `mail.smtpserver.*` (`hostname`, `port`, `try_ssl`, `username`, `authMethod`) | Utgående server |
| `socketType`/`try_ssl` 0 / 2 / 3 | Ingen / STARTTLS / SSL/TLS |

Begränsningar:

- **Lösenord följer inte med åt något håll.** Thunderbird lagrar dem krypterat i `logins.json` och `key4.db`, och Thunderbirds egen import tar inte heller med dem.
- POP3, nyhetsgrupper, RSS och Lokala mappar hoppas över vid import och redovisas i förhandsgranskningen.
- Gmail-konton som använder OAuth2 i Thunderbird importeras med Google-inloggning, om den är aktiverad. Övriga konton med OAuth2 importeras med en varning, eftersom de då kräver ett app-lösenord.
- HTML-signaturer blir ren text. Signaturer som läses från fil (`attach_signature`) kan inte importeras.
- Mappinställningar (Skickat, Utkast), filter, adressböcker och e-post ingår inte. Thunderbird hittar själv kontots Skickat- och Utkast-mappar.

Exporten är provad mot Thunderbird 140 ESR: Thunderbird läste in konton, identiteter och servrar från filen. Den `prefs.js` som Thunderbird sedan skrev importerades tillbaka med identiskt resultat.

## Google-inloggning för Gmail

Med Google-inloggning ansluter Gmail- och Google Workspace-konton via OAuth 2.0 (SASL XOAUTH2 för IMAP och SMTP) i stället för med app-lösenord. Användaren klickar på **Logga in med Google** i kontoinställningarna. Appen sparar bara en refresh-token, krypterad med `SECRET_KEY`, och hämtar nya access-token automatiskt.

Google kräver att varje installation har ett eget OAuth-klient-ID. Så här skapar du ett (engångsjobb för administratören):

1. Gå till [Google Cloud Console](https://console.cloud.google.com/) och skapa ett projekt.
2. Under **Google Auth Platform → Branding** (OAuth-samtyckesskärmen) anger du appnamn och kontaktadress. Välj målgrupp:
   - **Internal** om alla användare finns i er Google Workspace-organisation. Då krävs ingen granskning.
   - **External** för vanliga Gmail-konton.
3. Under **Data Access** lägger du till behörigheten `https://mail.google.com/`.
4. Under **Clients** skapar du en klient av typen **Web application** med den auktoriserade omdirigerings-URI:n `<PUBLIC_URL>/api/oauth/google/callback`, till exempel `http://localhost:8080/api/oauth/google/callback`.
5. Sätt `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET` och `PUBLIC_URL` (se tabellen nedan) och starta om containern. I loggen står då `Google-inloggning för Gmail: aktiverad`.

Googles regler att känna till:

- `https://mail.google.com/` är en **begränsad behörighet**. En extern app som inte är granskad av Google visar en varning om att appen inte är verifierad, och den kan användas av högst 100 användare. För privat bruk eller en mindre grupp räcker det. För fler användare krävs Googles granskning, inklusive en säkerhetsrevision.
- Med publiceringsstatus **Testing** går inloggningen ut efter **7 dagar**, och bara tillagda testanvändare kan logga in. Byt till **In production** under **Audience** för att inloggningen ska fortsätta gälla.
- Omdirigerings-URI:n måste vara `http://localhost…` eller en **https**-adress med domännamn. Privata IP-adresser som `http://192.168.1.10` godkänns inte, så en server i hemnätverket behöver ett domännamn med HTTPS (till exempel via en omvänd proxy).
- Om Google återkallar behörigheten, till exempel för att användaren tagit bort appen i sitt Google-konto eller bytt lösenord, visar appen "logga in med Google igen". När ett konto tas bort, eller byts till lösenordsinloggning, återkallas behörigheten hos Google.

## Konfiguration

| Miljövariabel    | Standard      | Beskrivning |
|------------------|---------------|-------------|
| `ADMIN_USERNAME` | *(tom)*       | Skapar den första administratören vid start om det inte finns några användare. |
| `ADMIN_PASSWORD` | *(tom)*       | Lösenordet för administratören ovan (minst 8 tecken). |
| `SECRET_KEY`     | *(genereras)* | Nyckel för AES-256-GCM-krypteringen av e-postlösenord i databasen. Saknas den skapas `/data/secret.key`. Om nyckeln byts går sparade lösenord inte längre att läsa. |
| `PUBLIC_URL`     | *(förfrågans Host)* | Adressen som webbläsaren når appen på, t.ex. `https://mail.exempel.se`. Används för Googles callback-adress och bör alltid sättas bakom en omvänd proxy. |
| `GOOGLE_CLIENT_ID` | *(tom)*     | Klient-ID för Google-inloggning. Utan det och `GOOGLE_CLIENT_SECRET` är Google-inloggning avstängd. |
| `GOOGLE_CLIENT_SECRET` | *(tom)* | Klienthemligheten för klient-ID:t ovan. |
| `TBK_PORT`       | `8080`        | Port i containern. |
| `TBK_DATA_DIR`   | `/data`       | Plats för SQLite-databasen (volym). |

Konton och inställningar ligger i volymen `tbk-data`. Själva e-posten ligger kvar på e-postservern.

### Säkerhet

- Containern serverar vanlig HTTP. Om tjänsten ska nås utanför din egen dator, lägg en omvänd proxy med HTTPS framför (Caddy, Traefik, nginx).
- Inloggningslösenord lagras som scrypt-hashar. E-postlösenord kan inte hashas, eftersom de måste skickas till IMAP- och SMTP-servern, så de krypteras i stället med `SECRET_KEY`.
- Varje användare ser bara sina egna e-postkonton. Andras konton ger 404, även för administratörer.
- Sessioner sparas i databasen (bara tokenens SHA-256) och gäller i 14 dagar. Den som byter lösenord loggas ut på andra enheter, och en användare vars lösenord återställs av admin loggas ut överallt.
- Efter 10 misslyckade inloggningar på 15 minuter spärras användarnamnet tillfälligt (30 försök per IP-adress).
- Inloggningen ger en `HttpOnly`-cookie med `SameSite=Strict`. Alla ändrande API-anrop kräver dessutom headern `X-TBK: 1`, som skydd mot CSRF.
- Bilagor laddas alltid ned som filer, med undantag för PNG, JPEG, GIF och WebP, så att en bilaga inte kan köra skript i appens ursprung.

### Kända begränsningar

- **Microsoft Outlook.com och Microsoft 365** har stängt av inloggning med vanligt lösenord och kräver OAuth2, som inte stöds ännu.
- **Gmail** kan logga in med Google när Google-inloggning är konfigurerad, annars krävs ett app-lösenord. **iCloud och Yahoo** kräver ett app-lösenord när tvåstegsverifiering är aktiverad.
- Meddelanden skrivs som ren text. HTML-redigering finns inte ännu.
- Ingen adressbok eller kalender.
- Listan hämtas direkt från servern och cachas inte lokalt, så det finns inget offlineläge.
- Meddelanden kan inte flyttas mellan olika konton.
- Många SMTP-servrar (till exempel Gmail och Microsoft 365) vägrar eller skriver om avsändaradresser som inte hör till det inloggade kontot. Välj en egen SMTP-server för avsändare på andra domäner.
- Kopian av ett skickat meddelande sparas i Skickat hos det konto som avsändaren tillhör, även när en egen SMTP-server används.

## Arkitektur

```
Webbläsare (Vue 3 + Pinia)
        │  JSON över HTTP (/api/…)
        ▼
tbk-server (C++20, cpp-httplib)
  ├─ api.cpp           REST-endpoints, behörighet per användare, CSRF-kontroll
  ├─ mail_service.cpp  En IMAP-anslutning per konto, återanvänds mellan anrop
  ├─ imap.cpp          Egen IMAP4rev1-klient (literaler, pipelining, MOVE/UIDPLUS, mUTF-7)
  ├─ smtp.cpp          Sändning via libcurl
  ├─ thunderbird_prefs.cpp  Tolkning och skrivning av Thunderbirds prefs.js, zip-export
  ├─ mime.cpp          Tolkning och byggande av meddelanden med GMime 3
  ├─ auth.cpp          Användare, scrypt, sessioner, spärr mot lösenordsgissning
  ├─ oauth.cpp         Google OAuth 2.0 med PKCE, tokenutbyte, förnyelse och återkallelse
  ├─ db.cpp            SQLite: användare, sessioner, e-postkonton (krypterade lösenord), schemamigrering
  └─ net.cpp           TCP/TLS med OpenSSL, inklusive STARTTLS och certifikatverifiering
        │
        ▼
IMAP- och SMTP-servrar
```

Dockerfilen bygger i tre steg: Node bygger frontenden, Debian bygger C++-binären och en slimmad Debian-image kör binären. Binären serverar både API:t och de statiska frontendfilerna.

### Utveckling av frontenden

Kör backenden i Docker och Vite lokalt med hot reload. Vite skickar `/api` vidare till port 8080.

```bash
docker compose --profile dev up -d --build
cd frontend
npm install
npm run dev
```

### REST-API i korthet

| Metod | Sökväg | Beskrivning |
|-------|--------|-------------|
| GET/POST | `/api/session`, `/api/setup`, `/api/login`, `/api/logout`, `POST /api/me/password` | Installation, inloggning och eget lösenord |
| GET/POST/PUT/DELETE | `/api/users[/:id]` | Användarhantering (endast administratör) |
| GET/POST/PUT/DELETE | `/api/accounts[/:id]`, `POST /api/accounts/test` | Konton (listan innehåller `identities`) |
| POST/PUT/DELETE | `/api/accounts/:id/identities[/:identityId]`, `POST …/identities/test` | Extra avsändare |
| POST | `/api/import/thunderbird/preview` (`{ "prefs": "<innehållet i prefs.js>" }`) | Förhandsgranskning av import från Thunderbird |
| GET | `/api/export/thunderbird` | Zip-fil med `prefs.js` för Thunderbird |
| GET/POST | `/api/oauth/providers`, `POST /api/oauth/google/start`, `GET /api/oauth/google/status?state=` | Google-inloggning. Kontot sparas sedan med `authType: "oauth-google"` och `oauthState` |
| GET | `/api/oauth/google/callback` | Omdirigeringsadress som registreras hos Google |
| GET/POST | `/api/accounts/:id/folders`, `…/folders/rename`, `…/folders/delete` | Mappar |
| GET | `/api/accounts/:id/messages?folder=&page=&pageSize=&q=&unread=1&flagged=1` | Meddelandelista |
| GET | `/api/accounts/:id/message?folder=&uid=` | Ett meddelande |
| GET | `/api/accounts/:id/attachment?folder=&uid=&index=`, `…/source?folder=&uid=` | Bilaga och källkod |
| POST | `/api/accounts/:id/messages/flags`, `…/move`, `…/delete` | Åtgärder |
| POST | `/api/accounts/:id/send`, `/api/accounts/:id/drafts` | Skicka och spara utkast (multipart: `message` + `attachments`; `identityId` i `message` väljer avsändare) |
