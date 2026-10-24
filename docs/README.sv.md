# PigeonPost – handbok på svenska

> Det här är den utförliga dokumentationen på svenska. Installation och konfiguration i korthet finns i [README.md](../README.md) på engelska.
>
> Gränssnittet är på engelska som standard; svenska väljs på inloggningssidan eller under Layout-inställningarna (valet sparas i webbläsaren). Handbokens skärmtexter anges på svenska.

En webbaserad e-postklient i Thunderbirds anda (tidigare Brevduva; containrar, miljövariabler och API-headern `X-Brevduva` har kvar det gamla namnet). Backenden är skriven i C++20 och frontenden i Vue 3. Appen körs i en Docker-container och sparar användare och konton i MariaDB, som startas i en egen container.

Klienten ansluter till riktiga e-postkonton via IMAP och SMTP, till exempel Gmail, iCloud eller en egen server.

## Funktioner

- Flera användare, där var och en har sina egna e-postkonton. En administratör skapar användare och kan återställa lösenord
- Flera e-postkonton per användare, med mappträd och antal olästa per mapp
- Flera adresser kan läggas till på en gång när de delar serverinställningar och lösenord, antingen som egna konton eller som avsändare i ett konto. Du ser resultatet per adress
- Gmail-konton kan logga in med Google (OAuth 2.0), så inget app-lösenord behövs (se [Google-inloggning för Gmail](#google-inloggning-för-gmail))
- Flera avsändare per konto (Thunderbirds "identiteter"), var och en med eget namn, egen adress, svarsadress och signatur. Signaturer kan vara formaterade, med till exempel färger, länkar och logotyp. Flera adresser kan läggas till på en gång när de delar inställningar. En avsändare kan använda kontots utgående server eller en egen SMTP-server med egen inloggning. Du väljer avsändare i Från-fältet, och vid svar väljs automatiskt den adress som mejlet skickades till
- Tre paneler: mappar, meddelandelista och läsruta. Under **Layout** väljer du var panelerna ska ligga: mappfältet till vänster, till höger eller dolt, och läsrutan under, ovanför, till höger eller till vänster om listan, eller dold. Storlekarna går att dra om, och valen sparas i webbläsaren. När läsrutan ligger bredvid listan visas meddelandena på två rader. Utan läsruta öppnas ett meddelande i ett eget fönster
- Backup av ett e-postkonto till en zip-fil, till ett annat IMAP-konto eller till Google Drive, direkt eller enligt schema (se [Backup](#backup))
- **Olästa**: en vy överst i mapplistan med alla olästa meddelanden i alla konton och mappar (utom papperskorg, skräppost, skickat och utkast), där varje rad visar mapp och konto. Läsa, stjärnmärka, flytta, arkivera och ta bort fungerar direkt i vyn, även för en markering med mejl från flera mappar och konton. Ett mejl du har läst försvinner ur vyn när du går vidare till nästa. Vyn läses ur [mejlcachen](#mejlcache)
- **Papperskorg som töms av sig själv**: borttagna meddelanden hamnar i kontots papperskorg och tas bort för gott efter det antal dagar du väljer under **Inställningar** i menyn (standard 30, eller aldrig). Tiden räknas från när meddelandet hamnade i papperskorgen, även när ett annat e-postprogram la det där, eftersom IMAP inte sparar någon flyttid (servern antecknar själv när den först såg meddelandet där). Inställningen gäller per användare och även i Android-appen.
- Sökning i alla mappar och konton på en gång, med filter på avsändare, mottagare, datum, mapp, olästa och bilagor. Med en AI-tjänst kan du beskriva vad du letar efter med egna ord (se [Sök i alla mappar](#sök-i-alla-mappar))
- Regler för inkommande post: villkor på avsändare, mottagare och ämne, med flytt, läst, stjärna, skräppost eller papperskorg som åtgärd. Reglerna körs på servern när posten kommer, utan AI, och kan köras på en hel mapp i efterhand. Servern föreslår regler utifrån hur du har sorterat hittills, och med en AI-tjänst kan den föreslå fler (se [Regler för inkommande post](#regler-för-inkommande-post))
- Sortering med AI: en AI-tjänst som Gemini föreslår mapp för varje meddelande, och du godkänner innan något flyttas. Ny post kan också sorteras automatiskt i bakgrunden, och servern kan gå igenom en hel mapp med tusentals meddelanden på egen hand, med historik och ångra. Sorteringen lär sig av dina rättningar, och dina regler går alltid före (se [Sortera med AI](#sortera-med-ai))
- Läsa text- och HTML-meddelanden. HTML visas i en sandlådad iframe utan skript, och fjärrinnehåll (spårningspixlar och liknande) blockeras tills du tillåter det, för det mejlet eller för alla mejl från avsändaren
- Inbäddade bilder (`cid:`) och nedladdning av bilagor
- Skriva, svara, svara alla och vidarebefordra (med originalets bilagor), med bilagor via knapp eller dra-och-släpp
- Formaterad text i skrivfönstret: rubriker, typsnitt, storlek, fet, kursiv, färger, listor, indrag, citat, justering, länkar, tabeller och infogade bilder. Med knappen **Formatering** växlar du till ren text, och valet sparas i webbläsaren (se [Formaterade meddelanden](#formaterade-meddelanden))
- Utkast: spara, öppna och redigera, skicka
- Kopia sparas i Skickat, och besvarade meddelanden märks
- Stjärnmärka, markera som läst eller oläst, arkivera, flytta till skräppost, ta bort (till papperskorgen, eller permanent när du redan är i papperskorgen)
- Flytta meddelanden genom att dra dem till en mapp
- Import och export av konton, avsändare, servrar och signaturer i Thunderbirds format (`prefs.js`), se nedan
- Mejlcache i databasen: mappar, listor, läsning och sökning går mot en lokal kopia, vilket går fort och fungerar även när mejlservern inte går att nå (se [Mejlcache](#mejlcache))
- Snabbfilter: fritext (ämne, från, till och meddelandets text), olästa, stjärnmärkta
- Sortering: nyast först från början. Klicka på en kolumnrubrik (**Ämne**, **Korrespondent**/**Mottagare** eller **Datum**) för att sortera på den, och en gång till för att vända ordningen. I den tvåradiga listan, där rubrikerna inte syns, väljer du sortering i filterraden, där även storlek går att välja. Vyn **Olästa** och sökningen i alla mappar har sina egna sorteringar. Valen sparas i webbläsaren. Ämnen sorteras utan svarsprefix (Re:, Sv:, Fwd:) och avsändare på namnet
- Skapa, byta namn på, flytta och ta bort mappar (högerklick). En mapp flyttas också genom att dra den till en annan mapp, eller till kontot för att lägga den på översta nivån. Undermappar och meddelanden följer med
- Hämtar nya meddelanden automatiskt: servern kontrollerar varje konto var 20:e sekund och webbläsaren uppdaterar listan var 30:e
- Mörkt tema om operativsystemet använder det
- Kortkommandon: `Ctrl+N` skriv, `Ctrl+R` svara, `Ctrl+Shift+R` svara alla, `Ctrl+L` vidarebefordra, `Delete` ta bort, `↑/↓` bläddra, `S` stjärna, `M` läst/oläst, `A` arkivera, `J` skräppost, `F5` hämta, `F8` visa/dölj läsrutan, `F9` visa/dölj mappfältet. I skrivfönstret: `Ctrl+Enter` skicka, `Ctrl+S` spara utkast, `Ctrl+B`/`Ctrl+I`/`Ctrl+U` fet, kursiv och understruken, `Ctrl+K` länk

## Kom igång

```bash
# Första administratören, en krypteringsnyckel för sparade e-postlösenord och lösenordet till databasen
cat > .env <<EOF
ADMIN_USERNAME=admin
ADMIN_PASSWORD=ett-starkt-lösenord
SECRET_KEY=$(openssl rand -hex 32)
DB_PASSWORD=$(openssl rand -hex 24)
EOF

docker compose pull && docker compose up -d   # eller: docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build
```

Spara `.env`, eftersom `SECRET_KEY` och `DB_PASSWORD` måste vara desamma vid varje start. MariaDB sätter lösenordet bara när databasen skapas första gången. Om du vill byta det senare gör du det med `ALTER USER` i databasen och i `.env`.

Öppna sedan http://localhost:8080 och logga in som administratör. Lägg till användare under **Meny → Hantera användare**. Varje användare lägger sedan till sina egna e-postkonton, och kontoguiden fyller i serverinställningar för Gmail, Outlook, iCloud och Yahoo utifrån e-postadressen.

Utan `ADMIN_USERNAME` och `ADMIN_PASSWORD` visas en installationssida första gången, och den som först öppnar appen blir administratör. Gör det innan tjänsten nås av andra. Variablerna används bara så länge databasen saknar användare, och du kan ta bort dem efteråt.

På Windows (PowerShell) kan du i stället sätta variablerna med `$env:DB_PASSWORD = '...'`.

### Uppgradera från SQLite-versionen

Tidigare versioner sparade allt i SQLite (`/data/brevduva.db`). Nu används MariaDB, och den gamla databasen läses inte in automatiskt. Efter uppgraderingen skapar du en administratör på nytt och lägger till e-postkontona igen. Själva e-posten ligger kvar på e-postservern.

### Testa utan riktigt konto

Profilen `dev` startar även en lokal testmailserver (GreenMail) som accepterar alla användarnamn och lösenord:

```bash
docker compose --profile dev up -d
```

Lägg till ett konto med förinställningen **Testserver**, till exempel adressen `test@localhost` med lösenordet `test`. Skicka ett meddelande till `test@localhost`, så hamnar det i inkorgen. GreenMail har bara en inkorg från början. Skapa mapparna `Sent`, `Drafts`, `Trash` och `Arkiv` via högerklick på kontot för att prova hela flödet.

## Flytta inställningar till och från Thunderbird

Under **Meny → Importera/exportera (Thunderbird)**:

- **Exportera** laddar ned en zip-fil med `prefs.js` i roten, samma upplägg som Thunderbirds egen profilexport. I Thunderbird väljer du **Verktyg → Importera → Thunderbird**, pekar på zip-filen och bockar i konton och inställningar. Filen innehåller IMAP-konton, extra avsändare (identiteter), utgående servrar, svarsadresser och signaturer.
- **Importera** tar emot en export från Thunderbird (**Verktyg → Exportera**) eller `prefs.js` direkt från profilmappen. Zip-filen läses i webbläsaren, även stora ZIP64-arkiv med all e-post, och bara `prefs.js` skickas till servern. Du ser en förhandsgranskning, väljer konton, anger lösenord och kan testa anslutningarna.

Så översätts inställningarna:

| Thunderbird | PigeonPost |
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
- HTML-signaturer följer med åt båda håll som formaterade signaturer. Vid import tvättas HTML:en innan den används. Signaturer som läses från fil (`attach_signature`) kan inte importeras.
- Mappinställningar (Skickat, Utkast), filter, adressböcker och e-post ingår inte. Thunderbird hittar själv kontots Skickat- och Utkast-mappar.

Exporten är provad mot Thunderbird 140 ESR: Thunderbird läste in konton, identiteter och servrar från filen. Den `prefs.js` som Thunderbird sedan skrev importerades tillbaka med identiskt resultat.

## Backup

Under **Meny → Backup av e-postkonton** (eller högerklick på ett konto → **Backup…**) säkerhetskopierar du ett konto. Du väljer vilka mappar som ska ingå; mappar som skapas senare kommer med automatiskt. Gmails **Alla mail** och **Stjärnmärkt** är avmarkerade från början, eftersom de bara innehåller kopior av meddelanden i andra mappar.

| Mål | Så fungerar det |
|-----|-----------------|
| **Zip-fil** | Servern hämtar alla meddelanden och bygger en zip-fil som du laddar ned under **Körningar**. Filen sparas i 7 dagar. |
| **Annat IMAP-konto** | Meddelandena kopieras till ett av dina andra konton, med flaggor (läst, stjärna, besvarat, utkast) och ursprunglig mottagningstid. Med en målmapp, till exempel `Backup/namn@exempel.se`, hamnar kontots mappar under den. Med tom målmapp hamnar inkorgen i målkontots inkorg och Skickat, Utkast, Papperskorg, Skräppost och Arkiv i motsvarande mappar. Meddelanden som redan finns i målmappen (samma Message-ID) hoppas över, så en schemalagd backup kopierar bara det som är nytt. |
| **Google Drive** | Samma zip-fil laddas upp till mappen `Brevduva-backup` i Google Drive. Uppladdningen sker i bitar och fortsätter där den var om anslutningen bryts. För ett schema anger du hur många backuper som ska behållas; äldre tas bort. |

**Schema:** Backuper till Google Drive och IMAP-konton kan köras varje dag, varje vecka eller varje månad vid en viss tid. Tiden gäller den tidszon webbläsaren hade när schemat sparades, även vid sommartid. Om servern var avstängd vid den schemalagda tiden körs backupen en gång när servern startar igen. Under **Sparade backuper** kan du köra, pausa, ändra och ta bort scheman, och under **Körningar** ser du förlopp, resultat och varningar.

**Zip-filens innehåll:** en mapp per e-postmapp med ett meddelande per fil (`000123 Ämne.eml`) och `brevduva-backup.json` med mappar, flaggor, mottagningstider och Message-ID. Filen är en vanlig zip (ZIP64 för stora konton) som Windows, macOS, 7-Zip och `unzip` kan packa upp. `.eml`-filerna kan öppnas i de flesta e-postprogram, och i Thunderbird kan tillägget ImportExportTools NG importera en hel mapp.

Så här ansluter du Google Drive:

1. Google-inloggning måste vara konfigurerad (se nästa avsnitt). I samma Google Cloud-projekt aktiverar du **Google Drive API** under **APIs & Services → Library**.
2. Under **Google Auth Platform → Data Access** lägger du till behörigheten `https://www.googleapis.com/auth/drive.file`. Den ger bara åtkomst till filer som appen själv skapar och är ingen begränsad behörighet.
3. Välj **Google Drive** som mål i backupdialogen och klicka på **Anslut Google Drive**. Google Drive-kontot behöver inte vara samma som e-postkontot.

Att tänka på:

- Zip-filen byggs på servern i `/data/backups` (volymen `brevduva-data`), även när den ska till Google Drive. Det behövs alltså ledigt utrymme ungefär lika stort som kontots e-post under tiden backupen körs.
- Två backuper körs samtidigt; fler väntar i kö. Backuperna använder egna IMAP-anslutningar, så e-postklienten fungerar som vanligt under tiden.
- Gmail begränsar hur mycket som får laddas ned och upp via IMAP per dygn (några gigabyte). En första backup av ett stort Gmail-konto kan därför behöva köras flera gånger; till ett IMAP-konto fortsätter varje körning där den förra slutade.
- Egna nyckelord (till exempel `$Forwarded`) kopieras inte till ett annat IMAP-konto, eftersom alla servrar inte tillåter dem. De finns med i zip-filens `brevduva-backup.json`.
- Ett konto eller en Google Drive-koppling som används av en sparad backup kan inte tas bort förrän backupen ändrats eller tagits bort.
- Återställning från en zip-fil finns inte i appen ännu. Använd en backup till ett IMAP-konto om du vill kunna flytta tillbaka meddelanden direkt.

## Mejlcache

PigeonPost speglar varje kontos mappar, rubriker, flaggor och meddelandetext (text och HTML, inte bilagor) i MariaDB. Webbgränssnittet läser därifrån, så listor, läsning och sökning tar millisekunder i stället för en rundtur till mejlservern, och fungerar även när mejlservern är nere, seg eller har nått sin kvot.

- **Synk:** servern kontrollerar varje konto var 20:e sekund med egna IMAP-anslutningar. Ny post, flaggor och borttagningar från andra klienter förs in, och allt stäms av i sin helhet med jämna mellanrum (inkorgen var 10:e minut, övriga mappar varje timme). Knappen **Hämta** synkar direkt.
- **Första synken** hämtar rubrikerna för alla mappar, nyast först, och därefter texten. Under tiden visas förloppet under kontot i mapplistan, och det som inte hunnit sparas läses direkt från mejlservern som förr. Ett Gmail-konto med tiotusentals mejl kan ta några timmar, eftersom Gmail begränsar hur mycket som får hämtas via IMAP per dygn.
- **Sökning** går i ämne, avsändare, mottagare och meddelandets text (fulltextindex). Varje sökord ska finnas med och matchar början av ord. Ord med färre än tre tecken söks bara i ämne och adresser.
- **Sortering** görs i databasen. En mapp som inte hunnit in i cachen (eller väntar på avstämning efter en ändring) sorteras av mejlservern om den stöder IMAP-tillägget `SORT`; annars visas den i ankomstordning, som för det mesta är datumordning. Gmail saknar `SORT`, så dess virtuella mappar visas alltid i ankomstordning.
- **Virtuella mappar** (Gmails *Alla meddelanden*, *Stjärnmärkt* och *Viktigt*) cachas inte. De visar mejl som redan ligger i andra mappar, och skulle annars ge dubbletter i sökningen och i vyn över olästa. De läses direkt från mejlservern.
- **Ändringar** (flytta, ta bort, flagga, skicka) görs som förut direkt mot mejlservern, och cachen uppdateras samtidigt. Listan i webbgränssnittet uppdateras på en gång och anropet går i bakgrunden; misslyckas det visas felet och listan läses in igen. Ändringarna kräver alltså att mejlservern går att nå. När den inte gör det visas en rad under kontot, och läsning och sökning fortsätter att fungera på det som är sparat.
- **Bilagor och inbäddade bilder** hämtas från mejlservern när de behövs. Hela meddelanden som öppnats sparas som filer i `/data/cache/raw` (högst `RAW_CACHE_MB`, standard 1 GB; de äldsta tas bort först), så att bilagor i nyligen lästa meddelanden går att öppna igen utan mejlservern.
- **Stora meddelanden** (över 512 kB) hämtas bara till en del vid synken: texten sparas och är sökbar, men listan över bilagor blir fullständig först när meddelandet har öppnats en gång med kontakt med mejlservern.
- Gmails **Alla mail** och **Stjärnmärkt** cachas inte, eftersom de bara innehåller kopior. De läses direkt från mejlservern.
- **Lagring:** HTML och text komprimeras. Räkna med 5–15 kB per meddelande, alltså några hundra MB för en brevlåda med 50 000 meddelanden. Texten lagras **okrypterad**, så att den går att söka i; den som kommer åt databasen kan läsa den. Cachen tas bort tillsammans med kontot.
- **Ändringslista för appen:** varje skrivning i cachen får ett stigande ändringsnummer (`modseq`), och borttagna meddelanden lämnar en gravsten i 14 dagar. Android-appen hämtar bara det som ändrats sedan dess senaste nummer med `GET /api/accounts/:id/changes`, se [REST-API](#rest-api-i-korthet).
- `TRASH_PURGE_INTERVAL` (standard 900 sekunder) styr hur ofta papperskorgarna gås igenom för tömning; `TRASH_DAY_SECONDS` (standard 86 400) är bara till för testerna.
- `RULE_CHECK_SECONDS` (standard 300) är hur ofta reglerna för inkommande post letar efter ny post när cachen inte har sagt till.
- `MAIL_CACHE=0` stänger av cachen helt. Då läses allt direkt från mejlservern som i tidigare versioner.

## Formaterade meddelanden

Skrivfönstret har ett formateringsfält som i Thunderbird. Ett formaterat meddelande skickas som `multipart/alternative` med en HTML-del och en textversion, så att mottagare som läser ren text får en läsbar variant där citat är markerade med `>`.

- **Bilder** infogas med bildknappen eller klistras in från urklipp. Foton bredare än 1600 bildpunkter skalas ned. När meddelandet skickas bäddas bilderna in som egna delar (`multipart/related` med `cid:`-referenser), eftersom bland annat Gmail inte visar bilder som ligger som data-URI i HTML-koden. Filer som dras till fönstret blir vanliga bilagor.
- **Svar och vidarebefordran** citerar originalets HTML med formatering och inbäddade bilder. Innehållet tvättas först med [DOMPurify](https://github.com/cure53/DOMPurify): skript, formulär, stilmallar och fjärrbilder tas bort, så att citatet varken kan köra kod i appen eller avslöja för avsändaren att du har öppnat mejlet. Samma tvätt används för inklistrad HTML och för utkast som öppnas igen.
- **Tabeller** infogas med tabellknappen, där du pekar ut storleken i ett rutnät. Med markören i en tabell ger samma knapp rader och kolumner att infoga eller ta bort. `Tabb` går till nästa cell och ger en ny rad efter den sista. Tabellerna får inline-stilar (ramar och utfyllnad), eftersom e-postprogram inte läser stilmallar.
- **Signaturer** kan vara formaterade: bocka i **Formaterad signatur** under kontots eller avsändarens inställningar. En logotyp i signaturen skickas som inbäddad bild. Signaturens textversion sparas samtidigt och används i meddelanden i ren text.
- Redigeringsytan är alltid ljus, även i mörkt tema, så att färger ser ut som hos mottagaren.
- Ett utkast som sparats i ren text öppnas i ren text. Signaturen byts när du byter avsändare, i båda lägena.

## Sortera med AI

Under **Meny → Sortera med AI** (eller högerklick på en mapp eller på markerade meddelanden → **Sortera med AI…**) låter du en AI-tjänst föreslå vilken mapp varje meddelande hör hemma i. Inget flyttas förrän du har godkänt: du får en lista med föreslagen mapp och en kort motivering per meddelande, kan byta mapp eller bocka ur rader, och klickar sedan på **Flytta**. Meddelanden som AI:n är osäker på föreslås ligga kvar.

**Sortera en hel mapp:** en stor inkorg går du igenom i omgångar på upp till 500 meddelanden. Godkänn förslagen och klicka på **Flytta och hämta nästa**, så hämtas de närmast äldre meddelandena, även om en del av de förra fick ligga kvar. Servern minns per mapp hur långt bakåt du har kommit, så du kan stänga dialogen och välja **Fortsätt bakåt där du slutade** en annan dag eller på en annan enhet. Avbryts en omgång, eller tar AI-tjänstens kvot slut, räknas bara det AI:n hann med, och resten kommer i nästa omgång. **Börja om från början** nollställer mappen. Post som kommer in under tiden tas med **De senaste i mappen** eller av den automatiska sorteringen.

Så här kommer du igång:

1. Skapa en API-nyckel hos tjänsten, för Gemini i [Google AI Studio](https://aistudio.google.com/apikey). Det är en annan nyckel än OAuth-klienten för Google-inloggning.
2. Öppna fliken **Inställningar** i dialogen, välj tjänst, klistra in nyckeln och klicka på **Testa**. Testet hämtar tjänstens modeller, som sedan går att välja i fältet **Modell**. Förvalet för Gemini är `gemini-flash-latest`, som alltid pekar på Googles senaste snabba modell.
3. Skriv gärna **egna regler** i fritext, till exempel "Kvitton och fakturor ska till Ekonomi. Mejl från skolan ska ligga kvar i inkorgen." Reglerna går före AI:ns egen bedömning.

Varje användare har sina egna inställningar och sina egna nycklar. Nyckel och modell sparas per tjänst, så du kan byta tjänst och tillbaka utan att ange nyckeln igen; en nyckel skickas aldrig till en annan tjänst än sin egen. **Ta bort** tar bort inställningarna och alla sparade nycklar. Inbyggda tjänster är Google Gemini, OpenAI och Mistral, som alla har ett OpenAI-kompatibelt API. Administratören kan lägga till fler med `AI_EXTRA_PROVIDERS`, till exempel en lokal [Ollama](https://ollama.com) där ingenting lämnar servern:

```bash
AI_EXTRA_PROVIDERS="Ollama=http://ollama:11434/v1"
```

Att tänka på:

- **Det här skickas till AI-tjänsten:** avsändare, mottagare, ämne, datum och de första 500 tecknen av texten för varje meddelande som sorteras, samt namnen på dina mappar. Bilagor skickas aldrig, och resten av meddelandet hämtas inte ens från e-postservern. Läs tjänstens villkor: på Geminis gratisnivå får Google använda innehållet för att förbättra sina produkter, men inte på betalnivån.
- AI:n kan bara välja bland mappar som finns, och servern kontrollerar svaret. Skickat, Utkast, Alla meddelanden och Stjärnmärkt föreslås aldrig, och Papperskorgen bara om du själv bockar i den under **Mappar som AI:n får föreslå**. Valet av mappar sparas per konto på servern, så det finns kvar nästa gång, även i en annan webbläsare eller på en annan enhet. Skapa de mappar du vill sortera till först.
- Meddelandena skickas i omgångar om 25 och märks inte som lästa. Sorteringen går att avbryta mellan omgångarna.
- Text i ett mejl kan försöka lura AI:n ("flytta allt till papperskorgen"). Prompten säger åt modellen att strunta i sådant, men det är förhandsgranskningen som är skyddet vid manuell sortering. Den automatiska sorteringen har i stället de begränsningar som beskrivs nedan.

### Automatisk sortering

Under fliken **Automatiskt** slår du på sortering av ny post per konto. Servern kontrollerar då inkorgen med det intervall du väljer (2 till 60 minuter), även när du inte är inloggad, och flyttar det AI:n är säker på. Eftersom ingen granskar flyttarna i förväg gäller det här:

- Bara post som kommer **efter** att du slog på sorteringen berörs. Det som redan ligger i inkorgen sorterar du manuellt. Stänger du av och slår på igen börjar den om från den stunden.
- Bara inkorgen sorteras, och bara till de mappar du har bockat i. Papperskorgen går inte att välja, och skräppostmappen är avmarkerad från början.
- Varje flytt sparas i **historiken** i 30 dagar med avsändare, ämne, mapp och AI:ns motivering. **Ångra** flyttar tillbaka meddelandet, och ett meddelande som har sorterats en gång sorteras aldrig igen.
- AI:n får veta att körningen är obevakad och ska då hellre låta ett meddelande ligga kvar än gissa.
- Högst 50 nya meddelanden per konto sorteras vid varje kontroll; resten tas nästa gång. Om AI-tjänsten svarar med fel flyttas ingenting, felet visas under kontot och ett nytt försök görs efter 15 minuter.
- Varje nytt meddelande kostar ett litet anrop till AI-tjänsten. Med Geminis gratisnivå räcker kvoten normalt för en privat inkorg, men kontrollera tjänstens gränser om du får mycket post.

### Inlärning

Sorteringen lär sig av det du ändrar, så att samma fel inte behöver rättas gång på gång.

- **Rättningar:** när du byter mapp på ett förslag, tackar nej till det (bockar ur det eller väljer *Ligger kvar*) eller ångrar en automatisk flytt sparas det som en rättning: avsändare, ämne, vad AI:n föreslog och vad du valde. Vid varje sortering får AI:n upp till 30 rättningar som exempel, först de för samma avsändare och domän som mejlen i omgången, sedan de senaste. Det gäller manuell sortering, automatisk sortering och hela mappar i bakgrunden. Inget nytt slags data lämnar servern: avsändare och ämne skickas redan vid sortering.
- **Regelförslag:** när du har gjort samma val tre gånger i rad för en avsändare föreslår dialogen en regel, till exempel "Mejl från kund@bredband.se → Ekonomi". Regeln hamnar bland [reglerna för inkommande post](#regler-för-inkommande-post), gäller före AI:n och kostar inget AI-anrop. **Nej tack** gör att förslaget inte kommer tillbaka.
- Utan granskning (automatiskt och i bakgrunden) följs reglerna som alltid, men aldrig till papperskorgen: sådana mejl ligger kvar tills du sorterar dem själv.
- Under **Inställningar → Inlärt** ser du rättningarna och kan ta bort dem. Högst 5 000 beslut sparas per konto. Rättningarna följer med när en mapp byter namn.

### Hela mappen i bakgrunden

Med **Sortera hela mappen i bakgrunden** går servern själv igenom mappen, utan att du granskar förslagen. Det passar en inkorg med tusentals meddelanden. Du kan stänga webbläsaren; jobbet fortsätter på servern, även efter en omstart.

- Samma försiktighet som den automatiska sorteringen: bara det AI:n är säker på flyttas, aldrig till papperskorgen, och dina egna regler används. Meddelanden utan Message-ID rörs inte, eftersom flytten då inte går att ångra.
- Jobbet tar 25 meddelanden i taget, nyast först, med några sekunders paus mellan omgångarna för AI-tjänstens kvot. Räkna med en till två timmar för 10 000 meddelanden. Tar kvoten slut görs ett nytt försök efter 15 minuter, och efter åtta fel i rad pausas jobbet.
- Förloppet visas överst på fliken **Sortera**, med antal flyttade per mapp. Jobbet går att **pausa**, **fortsätta** och **avbryta**. Ett konto har ett jobb i taget.
- Varje flytt hamnar i historiken under **Automatiskt** och går att ångra för sig. **Ångra allt** flyttar tillbaka allt jobbet har flyttat, också i bakgrunden, så länge historiken finns kvar (30 dagar). Efter **Ångra allt** går mappen att sortera igen, till exempel med bättre regler. En flytt du har ångrat för sig görs däremot aldrig om.
- Jobbet och den manuella sorteringen delar märke: **Fortsätt bakåt där du slutade** tar vid där jobbet slutade, och ett jobb som startas med det valet fortsätter där du själv slutade.

## Regler för inkommande post

Under **Meny → Regler** (eller högerklick på ett meddelande → **Skapa regel från meddelandet…**) gör du regler som körs på servern när ny post kommer till inkorgen, utan någon AI-tjänst och även när du inte är inloggad.

- **Villkor:** avsändare, mottagare eller ämne som *innehåller*, *är*, *börjar med* eller *slutar med* en text, eller en hel avsändardomän (`@exempel.se`). En regel kan ha upp till tio villkor, och du väljer om alla eller något av dem ska stämma. Jämförelsen bryr sig inte om stora och små bokstäver.
- **Åtgärder:** flytta till en mapp, markera som skräppost, ta bort (till papperskorgen), markera som läst, stjärnmärk, och **sluta efter den här regeln**. Reglerna körs i den ordning de står; flytta dem upp och ned i listan. En regel utan flytt som bara stoppar betyder "ligger kvar", vilket också AI-sorteringen respekterar.
- **Provkörning:** medan du skriver regeln visas hur många meddelanden i inkorgen den skulle träffa, med några exempel. Det kräver [mejlcachen](#mejlcache).
- **Ny post:** reglerna tar bara post som kommer efter att regeln sparades. Mejlcachens synk säger till så fort något nytt har kommit; utan cachen kontrolleras inkorgen var femte minut (`RULE_CHECK_SECONDS`). Högst 50 meddelanden per kontroll; resten tas direkt efter.
- **Kör på hela inkorgen:** **Kör reglerna nu** går igenom allt som ligger i inkorgen i bakgrunden, nyast först, och visar hur många som flyttades.
- **Historik och ångra:** varje flytt sparas i samma historik som AI-sorteringen i 30 dagar, med regelns namn. **Ångra** flyttar tillbaka meddelandet, och det sorteras då inte igen av regeln. Regeln följer med när en mapp byter namn; pekar den på en mapp som tagits bort ligger meddelandet kvar och felet syns i serverloggen.

**Förslag på regler.** Med **Föreslå regler** tittar servern på hur du redan har sorterat: avsändare med minst tre meddelanden som till minst 80 procent ligger i en och samma mapp (utom inkorg, skickat, utkast, skräppost och papperskorg) föreslås som regel till den mappen, och när flera avsändare från samma domän alla hamnar i samma mapp föreslås en domänregel i stället. Varje förslag visar hur många meddelanden som ligger i mappen, hur många som ligger någon annanstans och hur många som fortfarande ligger i inkorgen. **Nej tack** gör att avsändaren inte föreslås igen.

Med **Fråga också AI:n** (kräver en AI-tjänst under [Sortera med AI](#sortera-med-ai)) får AI-tjänsten se mapparnas namn, de vanligaste avsändarna per mapp och ett urval avsändare och ämnesrader per mapp och ur inkorgen, aldrig något innehåll. Den kan då föreslå regler på ämnesrader, till exempel ett nyhetsbrevs namn. Allt den föreslår kontrolleras som en vanlig regel och provkörs mot cachen: bara regler som träffar minst två meddelanden i mappen, och till minst 70 procent träffar rätt mapp, visas. Du avgör själv vilka förslag som blir regler.

## Sök i alla mappar

Sökfältet ovanför meddelandelistan söker i mappen du står i. Knappen **Alla mappar** bredvid (eller Skift+Enter i sökfältet, eller **Meny → Sök i alla mappar**) öppnar en sökning över alla konton och mappar, och tar med sig det du har skrivit.

- **Sökord:** alla ord ska finnas med, i ämne, avsändare, mottagare eller meddelandets text. Orden matchar början av ord, så `faktur` hittar faktura, fakturan och fakturor. Skriv `|` mellan alternativ (`kvitto|faktura|invoice`) och citattecken runt en fras (`"anna svensson"`).
- **Filter:** avsändare, mottagare, datumintervall, bara olästa, bara stjärnmärkta och med bilaga.
- **Konto och mapp:** från början söks alla konton och mappar. Välj ett konto för att söka i en viss mapp. Papperskorg och skräppost tas bara med om du bockar i det, eller väljer mappen.
- **Sortering:** nyast först från början. Under sökformuläret väljer du datum, avsändare, mottagare, ämne eller storlek, och stigande eller fallande ordning.
- Klicka på en träff för att öppna den i sin mapp. Träffarna finns kvar när du öppnar sökningen igen.

**Sök med AI:** har du ställt in en AI-tjänst under [Sortera med AI](#sortera-med-ai) kan du beskriva vad du letar efter, till exempel "fakturan från elbolaget i våras" eller "olästa mejl med bilaga från Anna förra veckan". AI:n gör om frågan till sökord med synonymer, avsändare, datum och mapp. Filtren fylls i och visas, så att du ser vad som söktes och kan ändra. Själva sökningen görs i databasen på servern: till AI-tjänsten skickas bara din fråga, dagens datum och kontonas adresser och mappnamn, aldrig några meddelanden.

Sökningen går mot [mejlcachen](#mejlcache) och hittar det som har hunnit synkas dit. Den kräver att cachen är påslagen (`MAIL_CACHE`, på från början).

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

## Android-app

PigeonPost har en Android-app (ett eget projekt, inte med i det här repot) som speglar mejlcachen i telefonen, så att mejl går att läsa utan uppkoppling, och som visar notiser när ny post kommer. Appen pratar med samma API som webbgränssnittet, med två tillägg:

- **Ändringslistan** `GET /api/accounts/:id/changes?since=` ger bara det som ändrats sedan appens senaste synk (nya och ändrade meddelanden, borttagna meddelanden och mapplistan), se [Mejlcache](#mejlcache).
- **Texter i bunt** `POST /api/accounts/:id/messages/bodies` med `{folder, uids}` ger upp till 100 meddelandetexter ur mejlcachen i ett svar (det som inte finns i cachen listas i `missing`), så att appen kan hämta en stor brevlåda utan ett anrop per mejl.
- **Push-notiser** skickas med Firebase Cloud Messaging när synken ser ny post i en mapp som telefonen bevakar (inkorgen från början; andra mappar väljs i appen). Servern skickar bara ett datameddelande med avsändare och ämne; appen ritar notisen och hämtar sedan ändringarna. Mejl som redan är lästa, ligger i Skickat, Utkast, Skräppost eller Papperskorgen, kom före förra synken (t.ex. flyttats in från en annan mapp) eller tillhör en första synk ger ingen notis, och ett mejl som flyttas mellan bevakade mappar ger inte en andra.

Så slår du på push:

1. Skapa ett Firebase-projekt (kan ligga i samma Google Cloud-projekt som Google-inloggningen) och aktivera **Firebase Cloud Messaging API (V1)**. Registrera en Android-app med paketnamnet `cc.pigeonpost.app` och ladda ned `google-services.json`, som behövs när appen byggs.
2. Skapa ett tjänstekonto med rollen **Firebase Cloud Messaging API Admin** och ladda ned dess JSON-nyckel.
3. Lägg nyckeln där containern når den (t.ex. monterad under `/run/secrets`), läsbar för användaren `brevduva` (uid 10001), och sätt `FCM_SERVICE_ACCOUNT_FILE` till sökvägen. Loggen visar vid start om push är aktiverad.
4. Ladda också ner projektets `google-services.json` (Projektinställningar → Dina appar, Android-appen med paketnamnet `cc.pigeonpost.app`) och sätt `FIREBASE_CONFIG_FILE` till dess sökväg. Appen hämtar projektets klientvärden från `GET /api/push/config` vid inloggning och initierar Firebase med dem, så den publicerade APK:n fungerar med push mot din server utan att byggas om. Värdena i filen är inte hemliga; tjänstekontonyckeln i steg 2 är det.

APK-filen och `version.json` läggs i `BREVDUVA_ANDROID_DIR` (standard `/data/android`, lämpligen en egen volym) och serveras utan inloggning under `/android`, så att appen kan hitta och hämta uppdateringar.

## Konfiguration

| Miljövariabel    | Standard      | Beskrivning |
|------------------|---------------|-------------|
| `ADMIN_USERNAME` | *(tom)*       | Skapar den första administratören vid start om det inte finns några användare. |
| `ADMIN_PASSWORD` | *(tom)*       | Lösenordet för administratören ovan (minst 8 tecken). |
| `SECRET_KEY`     | *(genereras)* | Nyckel för AES-256-GCM-krypteringen av e-postlösenord i databasen. Saknas den skapas `/data/secret.key`. Om nyckeln byts går sparade lösenord inte längre att läsa. |
| `PUBLIC_URL`     | *(förfrågans Host)* | Adressen som webbläsaren når appen på, t.ex. `https://mail.exempel.se`. Används för Googles callback-adress och bör alltid sättas bakom en omvänd proxy. |
| `GOOGLE_CLIENT_ID` | *(tom)*     | Klient-ID för Google-inloggning. Utan det och `GOOGLE_CLIENT_SECRET` är Google-inloggning avstängd. |
| `GOOGLE_CLIENT_SECRET` | *(tom)* | Klienthemligheten för klient-ID:t ovan. |
| `GOOGLE_API_URL` | `https://www.googleapis.com` | Bas för Google Drive API. Behöver bara ändras vid test mot en låtsasserver. |
| `AI_EXTRA_PROVIDERS` | *(tom)*   | Extra AI-tjänster för sortering, som `Namn=https://adress/v1` åtskilda med komma. Tjänsterna ska ha ett OpenAI-kompatibelt API och kräver ingen API-nyckel. Användare kan bara välja tjänster ur listan, så att servern inte kan fås att anropa godtyckliga adresser. |
| `OPERATOR_NAME`  | `the operator of this service` | Den som driver instansen, visas på `/privacy.html` och `/terms.html`. |
| `OPERATOR_CONTACT` | *(tom)*     | Kontakt (e-postadress eller webbadress) som visas på samma sidor. Egna sidor kan monteras som `/app/public/privacy.html` och `/app/public/terms.html`. |
| `MAIL_CACHE`     | `1`           | `0` stänger av mejlcachen, så att allt läses direkt från mejlservern. |
| `FCM_SERVICE_ACCOUNT_FILE` | – | JSON-nyckel för ett tjänstekonto i Firebase-projektet. Slår på push-notiser till Android-appen (se [Android-app](#android-app)). |
| `FIREBASE_CONFIG_FILE` | – | Projektets `google-services.json`. Appen hämtar klientvärdena därifrån via `/api/push/config`, så en och samma APK kan registrera sig mot vilken server som helst (se [Android-app](#android-app)). |
| `FCM_URL`, `FCM_TOKEN_URL` | Googles adresser | Byts bara ut mot en testserver (`tests/push-mock`). |
| `BREVDUVA_ANDROID_DIR` | `/data/android` | Katalog med appens APK och `version.json`, som serveras under `/android`. Saknas katalogen är `/android` avstängd. |
| `RAW_CACHE_MB`   | `1024`        | Högsta storlek i MB för nyligen öppnade meddelanden med bilagor i `/data/cache/raw`. `0` stänger av. |
| `TZ`             | `UTC`         | Tidszon för datum i filnamn på backuper i Google Drive, t.ex. `Europe/Stockholm`. Scheman använder alltid sin egen tidszon. |
| `BREVDUVA_PORT`       | `80`          | Port i containern. |
| `DB_PASSWORD`    | *(krävs)*     | Lösenord för databasanvändaren. I `docker-compose.yml` används det både av MariaDB och av appen. |
| `DB_HOST`, `DB_PORT` | `localhost`, `3306` | MariaDB-serverns adress. Compose sätter `db`. |
| `DB_NAME`, `DB_USER` | `brevduva`, `brevduva` | Databas och användare. |
| `BREVDUVA_DATA_DIR`   | `/data`       | Plats för `secret.key` (volym). |
| `BREVDUVA_BACKUP_DIR` | `/data/backups` | Zip-filer för backup medan de byggs, laddas upp eller väntar på nedladdning. |

Användare, sessioner, konton och inställningar ligger i MariaDB, i volymen `pigeonpost-db`. Volymen `pigeonpost-data` innehåller `secret.key` och backupfiler. Själva e-posten ligger kvar på e-postservern.

### Säkerhet

- Containern serverar vanlig HTTP. Om tjänsten ska nås utanför din egen dator, lägg en omvänd proxy med HTTPS framför (Caddy, Traefik, nginx).
- Inloggningslösenord lagras som scrypt-hashar. E-postlösenord kan inte hashas, eftersom de måste skickas till IMAP- och SMTP-servern, så de krypteras i stället med `SECRET_KEY`.
- Varje användare ser bara sina egna e-postkonton. Andras konton ger 404, även för administratörer.
- Fjärrinnehåll i HTML-mejl blockeras tills du väljer **Visa fjärrinnehåll** (bara det mejlet) eller **Visa alltid från** avsändaren (alla mejl från adressen, även framtida). Valet sparas per användare på servern och ångras med **Sluta lita på avsändaren** i ett mejl från adressen. Avsändaradresser går att förfalska, så ett förfalskat mejl från en betrodd adress kan ladda bilder och därmed avslöja att du har öppnat det. Skript körs aldrig, oavsett val.
- Sessioner sparas i databasen (bara tokenens SHA-256) och gäller i 14 dagar. Den som byter lösenord loggas ut på andra enheter, och en användare vars lösenord återställs av admin loggas ut överallt.
- Efter 10 misslyckade inloggningar på 15 minuter spärras användarnamnet tillfälligt (30 försök per IP-adress).
- Inloggningen ger en `HttpOnly`-cookie med `SameSite=Strict`. Alla ändrande API-anrop kräver dessutom headern `X-Brevduva: 1`, som skydd mot CSRF.
- Google Drive-kopplingar har bara behörigheten `drive.file` och sparas krypterade med `SECRET_KEY`, liksom e-postkontonas token. Google återkallar hela appens behörighet för ett Google-konto på en gång, så en token återkallas bara när varken ett e-postkonto eller en Drive-koppling längre använder det Google-kontot.
- API-nycklar för AI-sortering sparas krypterade med `SECRET_KEY` och lämnas aldrig ut av API:t. En nyckel följer inte med när användaren byter AI-tjänst, och AI-tjänstens adress måste finnas i serverns lista.
- Mejlcachen lagrar rubriker och meddelandetext okrypterat i databasen (se [Mejlcache](#mejlcache)). Skydda databasen därefter. Lösenord och token är fortfarande krypterade.
- Bilagor laddas alltid ned som filer, med undantag för PNG, JPEG, GIF och WebP, så att en bilaga inte kan köra skript i appens ursprung.

### Kända begränsningar

- **Microsoft Outlook.com och Microsoft 365** har stängt av inloggning med vanligt lösenord och kräver OAuth2, som inte stöds ännu.
- **Gmail** kan logga in med Google när Google-inloggning är konfigurerad, annars krävs ett app-lösenord. **iCloud och Yahoo** kräver ett app-lösenord när tvåstegsverifiering är aktiverad.
- Celler i en tabell kan inte slås ihop eller ändra bredd i formateringsfältet.
- Ingen adressbok eller kalender.
- Webbläsaren måste nå Brevduva-servern; appen fungerar inte i en webbläsare helt utan nät. Ändringar (flytta, ta bort, skicka) köas inte när mejlservern är onåbar.
- Meddelanden kan inte flyttas mellan olika konton.
- Många SMTP-servrar (till exempel Gmail och Microsoft 365) vägrar eller skriver om avsändaradresser som inte hör till det inloggade kontot. Välj en egen SMTP-server för avsändare på andra domäner.
- Kopian av ett skickat meddelande sparas i Skickat hos det konto som avsändaren tillhör, även när en egen SMTP-server används.

## Arkitektur

```
Webbläsare (Vue 3 + Pinia)
        │  JSON över HTTP (/api/…)
        ▼
brevduva-server (C++20, cpp-httplib)
  ├─ api.cpp           REST-endpoints, behörighet per användare, CSRF-kontroll
  ├─ mail_service.cpp  En IMAP-anslutning per konto, återanvänds mellan anrop. Läser ur mejlcachen när det går
  ├─ mail_cache.cpp    Mejlcache: synktrådar med egna IMAP- och databasanslutningar, läsning ur cachen, status
  ├─ raw_cache.cpp     Nyligen öppnade meddelanden med bilagor som filer på disk
  ├─ imap.cpp          Egen IMAP4rev1-klient (literaler, pipelining, MOVE/UIDPLUS, mUTF-7)
  ├─ smtp.cpp          Sändning via libcurl
  ├─ thunderbird_prefs.cpp  Tolkning och skrivning av Thunderbirds prefs.js, zip-export
  ├─ mime.cpp          Tolkning och byggande av meddelanden med GMime 3
  ├─ auth.cpp          Användare, scrypt, sessioner, spärr mot lösenordsgissning
  ├─ oauth.cpp         Google OAuth 2.0 med PKCE, tokenutbyte, förnyelse och återkallelse
  ├─ backup.cpp        Backupkö, schemaläggare och kopiering till zip, IMAP eller Google Drive
  ├─ gdrive.cpp        Google Drive API v3 med återupptagbar uppladdning
  ├─ ai_sort.cpp       AI-sortering: inställningar, prompt, textutdrag, kontroll av AI:ns förslag och tråden för automatisk sortering
  ├─ ai.cpp            Klient för AI-tjänster med OpenAI-kompatibelt API (libcurl)
  ├─ zip_writer.cpp    Zip-filer som skrivs direkt till disk (deflate, ZIP64)
  ├─ db_cache.cpp      Mejlcachens tabeller: mappar, rubriker, komprimerad text och fulltextsökning
  ├─ db.cpp            MariaDB: användare, sessioner, e-postkonton (krypterade lösenord), backup, schemamigrering
  └─ net.cpp           TCP/TLS med OpenSSL, inklusive STARTTLS och certifikatverifiering
        │
        ▼
IMAP- och SMTP-servrar            MariaDB (tjänsten db)
```

Dockerfilen bygger i tre steg: Node bygger frontenden, Debian bygger C++-binären och en slimmad Debian-image kör binären. Binären serverar både API:t och de statiska frontendfilerna.

### Utveckling av frontenden

Kör backenden i Docker och Vite lokalt med hot reload. Vite skickar `/api` vidare till port 8080.

```bash
docker compose --profile dev up -d
cd frontend
npm install
npm run dev
```

### REST-API i korthet

| Metod | Sökväg | Beskrivning |
|-------|--------|-------------|
| GET/POST | `/api/session`, `/api/setup`, `/api/login`, `/api/logout`, `POST /api/me/password` | Installation, inloggning och eget lösenord. `/api/session` visar `expiresAt` för sessionen |
| GET/PUT | `/api/settings` (`{ trashDays }`) | Inställningar per användare: efter hur många dagar meddelanden i papperskorgen tas bort (0 = aldrig, högst 3650). Samma värde finns i `user` från `/api/session` |
| POST | `/api/session/renew` | Byter till en ny session med full livslängd (14 dagar) och tar bort den gamla. Appen använder den så att en telefon som används regelbundet inte loggas ut |
| GET/POST/PUT/DELETE | `/api/users[/:id]` | Användarhantering (endast administratör) |
| GET/POST/PUT/DELETE | `/api/accounts[/:id]`, `POST /api/accounts/test` | Konton (listan innehåller `identities`) |
| POST/PUT/DELETE | `/api/accounts/:id/identities[/:identityId]`, `POST …/identities/test` | Extra avsändare |
| POST | `/api/import/thunderbird/preview` (`{ "prefs": "<innehållet i prefs.js>" }`) | Förhandsgranskning av import från Thunderbird |
| GET | `/api/export/thunderbird` | Zip-fil med `prefs.js` för Thunderbird |
| GET/POST | `/api/oauth/providers`, `POST /api/oauth/google/start`, `GET /api/oauth/google/status?state=` | Google-inloggning. Kontot sparas sedan med `authType: "oauth-google"` och `oauthState` |
| GET | `/api/oauth/google/callback` | Omdirigeringsadress som registreras hos Google |
| GET | `/api/backup` | Google Drive-kopplingar, sparade backuper och de senaste körningarna |
| POST | `/api/backup/runs` (`{ "config": {...} }`), `…/runs/:id/cancel`, `DELETE …/runs/:id`, `GET …/runs/:id/download` | Engångsbackup, avbryt, ta bort och ladda ned zip-fil |
| POST/PUT/DELETE | `/api/backup/jobs[/:id]` (`{ name, config, schedule, enabled }`), `POST …/jobs/:id/run` | Sparade backuper med schema |
| POST/DELETE | `/api/backup/drive/start`, `/api/backup/drive` (`{ "oauthState": "..." }`), `DELETE /api/backup/drive/:id` | Anslut och koppla bort Google Drive. Inloggningens status hämtas med `/api/oauth/google/status` |
| GET/PUT/DELETE | `/api/ai` (`{ baseUrl, model, apiKey, instructions }`), `POST /api/ai/test` | Inställningar för AI-sortering. Testet sparar inget och returnerar tjänstens modeller |
| GET | `/api/ai/auto` | Regler för automatisk sortering och historiken över flyttade meddelanden |
| PUT | `/api/accounts/:id/ai-auto` (`{ enabled, targets, intervalMinutes }`), `POST /api/ai/log/:id/undo` | Slå på eller av automatisk sortering för ett konto, och ångra en flytt |
| GET/POST | `/api/remote-senders`, `POST /api/remote-senders/delete` (`{ sender }`) | Avsändare vars fjärrinnehåll alltid visas. Alla svarar med hela listan |
| GET/PUT/POST | `/api/devices` (`{ token, name, platform, appVersion, notify }`), `POST /api/devices/delete` (`{ token }`) | Telefoner med Android-appen som får push-notiser. `notify` = `{ "<kontoId>": ["INBOX", …] }` väljer mappar per konto; ett konto som saknas ger notiser för inkorgen. Utelämnade fält behåller sitt värde. `GET` visar `pushEnabled` och enheterna utan token |
| GET/POST/DELETE | `/api/ai/learning`, `POST /api/accounts/:id/ai-feedback` (`{ folder, decisions: [{ email, name, subject, suggested, chosen }] }`), `POST /api/accounts/:id/ai-rules` (`{ sender, target, dismissed? }`), `DELETE /api/ai/rules/:id`, `DELETE /api/ai/examples[/:id]` | Det sorteringen har lärt sig: avsändarregler, regelförslag och rättningar. Alla svarar med hela läget |
| GET/POST | `/api/ai/jobs`, `POST /api/accounts/:id/ai-jobs` (`{ folder, targets, unreadOnly, fromStart }`), `POST /api/ai/jobs/:id/pause\|resume\|cancel\|undo` | Sortering av en hel mapp i bakgrunden: läge och förlopp, starta ett jobb, och pausa, fortsätta, avbryta eller ångra det |
| POST | `/api/search` (`{ accountId?, folder?, q, from, to, after, before, unread, flagged, attachments, includeTrash, page, pageSize, sort?, order? }`) | Sökning i mejlcachen över alla konton och mappar, eller ett konto och en mapp. `after`/`before` är unixtid. `sort` och `order` som för meddelandelistan. Varje träff har `accountId` och `folder` |
| POST | `/api/ai/search` (`{ question, today }`) | AI:n gör om en fråga till filter för `/api/search`: `{ q, from, to, after, before, unread, flagged, attachments, accountId, folder, explanation }` med datum som `ÅÅÅÅ-MM-DD`. Inga meddelanden skickas till AI-tjänsten |
| PUT | `/api/accounts/:id/ai-sort-targets` (`{ excluded }`), `/api/accounts/:id/ai-sort-cursor` (`{ folder, beforeUid, date }`) | Sparar bortvalda målmappar och hur långt bakåt en mapp har sorterats. Båda följer med i `GET /api/ai` |
| POST | `/api/accounts/:id/ai-sort` (`{ folder, uids, targets? }`, högst 25 `uids`) | Förslag på målmapp per meddelande: `{ suggestions: [{ uid, target, reason }] }`. Inget flyttas; godkända förslag flyttas med `…/messages/move` |
| GET/POST | `/api/accounts/:id/folders`, `…/folders/rename` (`{ from, to }`, används även för att flytta en mapp), `…/folders/delete` | Mappar |
| GET | `/api/accounts/:id/messages?folder=&page=&pageSize=&q=&unread=1&flagged=1&beforeUid=&sort=&order=` | Meddelandelista. `sort` är `date` (standard), `from`, `to`, `subject`, `size` eller `uid` (ankomstordning) och `order` är `desc` (standard) eller `asc`. `beforeUid` ger bara äldre meddelanden. `cached: true` när den kommer ur mejlcachen |
| GET/POST | `/api/cache`, `POST /api/accounts/:id/sync` | Mejlcachens läge per konto (`state`: `waiting`, `syncing`, `idle` eller `offline`, och förlopp), och synk av ett konto nu |
| GET | `/api/accounts/:id/changes?since=&limit=` | Ändringar i mejlcachen sedan ändringsnumret `since`, för en klient med egen kopia (Android-appen): `{ since, modseq, more, folders, messages, removed }`. `since=0` ger bara mapplistan och det aktuella numret; den första fyllningen görs med `/messages`. Raderna i `messages` har `folder` och `modseq`, `removed` är borttagna `{ folder, uid }`, och en mapp med `resetModseq` över `since` ska hämtas om. Högst `limit` rader (standard 500) per svar; `more` betyder att det finns fler efter `modseq`. 410 när `since` är äldre än de sparade gravstenarna (14 dagar) eller okänt; då hämtas kontot om |
| GET | `/api/accounts/:id/message?folder=&uid=` | Ett meddelande. Inbäddade bilder ligger inte i `html`; de pekas ut av `attachments[].contentId` och hämtas som bilagor |
| GET | `/api/accounts/:id/attachment?folder=&uid=&index=`, `…/source?folder=&uid=` | Bilaga och källkod |
| POST | `/api/accounts/:id/messages/flags`, `…/move`, `…/delete` | Åtgärder |
| POST | `/api/accounts/:id/send`, `/api/accounts/:id/drafts` | Skicka och spara utkast (multipart: `message` + `attachments`; `identityId` i `message` väljer avsändare, och `html` i `message` ger ett formaterat meddelande med `text` som alternativ) |
