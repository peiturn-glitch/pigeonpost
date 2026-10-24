// Swedish translations for src/components/AiSortDialog.vue. Keys are the English strings used in t().
export default {
  // Settings
  'AI settings saved': 'AI-inställningarna sparades',
  'Remove the AI settings and all saved API keys?': 'Ta bort AI-inställningarna och alla sparade API-nycklar?',
  'AI settings removed': 'AI-inställningarna togs bort',
  'The folder selection could not be saved: {error}': 'Valet av mappar kunde inte sparas: {error}',

  // Sorting
  'The whole folder has been sorted: there are no older messages left to sort.':
    'Hela mappen är genomgången: det finns inga äldre meddelanden kvar att sortera.',
  'There are no messages to sort in this folder.': 'Det finns inga meddelanden att sortera i mappen.',
  '1 message': '1 meddelande',
  '{n} messages': '{n} meddelanden',
  '1 folder': '1 mapp',
  '{n} folders': '{n} mappar',
  'Moved {messages} to {folders}': '{messages} flyttades till {folders}',
  'The decisions could not be saved for learning: {error}': 'Besluten kunde inte sparas för inlärningen: {error}',
  'Forget everything the sorting has learned from your corrections? Rules are kept.':
    'Ta bort allt som sorteringen har lärt sig av dina rättningar? Reglerna ligger kvar.',
  'stays in place': 'ligger kvar',

  // Background jobs
  'messages older than {date}': 'meddelanden äldre än {date}',
  'all messages': 'alla meddelanden',
  'Sort {scope} in {folder} in the background?': 'Sortera {scope} i {folder} i bakgrunden?',
  'The server goes through the folder without you reviewing the suggestions. Only what the AI is sure about is moved, never to the trash.':
    'Servern går igenom mappen utan att du granskar förslagen. Bara det AI:n är säker på flyttas, aldrig till papperskorgen.',
  'Every move is saved in the history under Automatic, and the whole run can be undone for {days} days.':
    'Varje flytt sparas i historiken under Automatiskt, och hela sorteringen går att ångra i {days} dagar.',
  'You can close this window; sorting continues on the server.': 'Du kan stänga fönstret; sorteringen fortsätter på servern.',
  'Move back the {n} messages that the sorting moved out of {folder}?':
    'Flytta tillbaka de {n} meddelanden som sorteringen har flyttat från {folder}?',
  'Cancel the sorting? Messages already moved stay where they are.': 'Avbryta sorteringen? Det som redan har flyttats ligger kvar.',
  Running: 'Pågår',
  Paused: 'Pausad',
  Done: 'Klar',
  Cancelled: 'Avbruten',
  'Moving back…': 'Flyttar tillbaka…',
  Undone: 'Ångrad',

  // Automatic sorting
  'Automatic sorting is on. New mail in the inbox will be sorted from now on.':
    'Automatisk sortering är på. Ny post i inkorgen sorteras från och med nu.',
  'Automatic sorting is off': 'Automatisk sortering är av',
  'The message was moved back and will not be sorted again': 'Meddelandet flyttades tillbaka och sorteras inte igen',

  // Dialog header and tabs
  'Sort with AI': 'Sortera med AI',
  Close: 'Stäng',
  Sort: 'Sortera',
  Automatic: 'Automatiskt',
  Settings: 'Inställningar',
  'Loading…': 'Läser in…',

  // Rule suggestions
  'Suggested rules': 'Förslag på regler',
  'Create all {n}': 'Skapa alla {n}',
  'You have made the same choice {n} times in a row for these senders. With a rule, their mail goes to the right place straight away, without asking the AI.':
    'Du har gjort samma val {n} gånger i rad för de här avsändarna. Med en regel hamnar deras mejl rätt direkt, utan att AI:n tillfrågas.',
  'Mail from': 'Mejl från',
  'Mail from {sender}': 'Mejl från {sender}',
  'Create rule': 'Skapa regel',
  'No thanks': 'Nej tack',
  '…and {n} more.': '…och {n} till.',
  'Choose an AI service and enter an API key to get started.': 'Välj en AI-tjänst och ange en API-nyckel för att komma igång.',
  'Open settings': 'Öppna inställningar',

  // Jobs list
  'Whole folders in the background': 'Hela mappar i bakgrunden',
  Pause: 'Pausa',
  Resume: 'Fortsätt',
  Cancel: 'Avbryt',
  'Undo all': 'Ångra allt',
  '{done} of {total} processed, {moved} moved': '{done} av {total} genomgångna, {moved} flyttade',
  '{n} messages could no longer be found and stay where they are.': '{n} meddelanden hittades inte längre och ligger kvar där de är.',
  'Retrying {time}.': 'Nytt försök {time}.',

  // Setup
  Account: 'Konto',
  'Folder to sort': 'Mapp att sortera',
  '{n} selected messages': '{n} markerade meddelanden',
  'will be sorted.': 'sorteras.',
  'Sort the latest in the folder instead': 'Välj de senaste i mappen i stället',
  'messages at a time': 'meddelanden i taget',
  'Unread only': 'Bara olästa',
  'The latest in the folder': 'De senaste i mappen',
  'Continue back where you left off (nothing in this folder has been sorted yet)':
    'Fortsätt bakåt där du slutade (inget är sorterat i mappen ännu)',
  'The whole folder has been sorted': 'Hela mappen är genomgången',
  'Continue back where you left off: older than {date}': 'Fortsätt bakåt där du slutade: äldre än {date}',
  '{n} left': '{n} kvar',
  'Start over from the beginning': 'Börja om från början',
  'To go through a whole folder: get suggestions, approve them and choose': 'För att gå igenom en hel mapp: hämta förslag, godkänn och välj',
  'Move and get next': 'Flytta och hämta nästa',
  'Each round takes the next older messages, even if some were left in place, and you can close and continue another day.':
    'Varje omgång tar de närmast äldre meddelandena, även om en del fick ligga kvar, och du kan stänga och fortsätta en annan dag.',
  'Folders the AI may suggest': 'Mappar som AI:n får föreslå',
  '{n} of {total}': '{n} av {total}',
  'There are no folders to sort into. Create folders by right-clicking the account, or select more above.':
    'Det finns inga mappar att sortera till. Skapa mappar genom att högerklicka på kontot, eller välj fler ovan.',
  'Sender, recipients, subject and the first {n} characters of the text are sent to {provider} ({model}). Nothing is moved until you have approved the suggestions.':
    'Avsändare, mottagare, ämne och de första {n} tecknen av texten skickas till {provider} ({model}). Inget flyttas förrän du har godkänt förslagen.',
  'Sorting uses {rules} rules and learns from {corrections} corrections (see Settings).':
    'Sorteringen använder {rules} regler och lär sig av {corrections} rättningar (se Inställningar).',
  'Your own rules are used.': 'Dina egna regler används.',
  'You can write your own rules under Settings.': 'Du kan skriva egna regler under Inställningar.',
  'This account already has a sort running in the background': 'Kontot har redan en sortering i bakgrunden',
  'The server sorts the whole folder without review. It can be paused and undone.':
    'Servern sorterar hela mappen utan granskning. Går att pausa och ångra.',
  'Starting…': 'Startar…',
  'Sort the whole folder in the background': 'Sortera hela mappen i bakgrunden',
  'Get suggestions': 'Hämta förslag',

  // Running
  'The AI is reading the messages… {done} of {total}': 'AI:n läser meddelandena… {done} av {total}',
  'Fetching messages…': 'Hämtar meddelanden…',
  '{n} suggestions so far': '{n} förslag hittills',

  // Review
  'of {total} messages are suggested to be moved. Change the target folder or untick what you disagree with.':
    'av {total} meddelanden föreslås flyttas. Ändra målmapp eller bocka ur det du inte håller med om.',
  'This round goes back to {date}; {n} older remain.': 'Omgången går tillbaka till {date}; {n} äldre återstår.',
  'Select all': 'Markera alla',
  From: 'Från',
  Subject: 'Ämne',
  'Move to': 'Flytta till',
  Reason: 'Motivering',
  '(no subject)': '(inget ämne)',
  '– Leave in place –': '– Ligger kvar –',
  'Changed by you': 'Ändrad av dig',
  Back: 'Tillbaka',
  'Moving…': 'Flyttar…',
  'Move 1 message': 'Flytta 1 meddelande',
  'Move {n} messages': 'Flytta {n} meddelanden',
  'Move {n} and get the next {limit}': 'Flytta {n} och hämta nästa {limit}',
  'Get the next {limit}': 'Hämta nästa {limit}',

  // Automatic tab
  'The server sorts new mail in the inbox even when you are not signed in. Only mail that arrives after you turn sorting on is affected, and the AI only moves what it is sure about. Nothing is moved to the trash, and every move can be undone in the history below. Your own rules under Settings are used.':
    'Servern sorterar ny post i inkorgen även när du inte är inloggad. Bara post som kommer efter att du slagit på sorteringen berörs, och AI:n flyttar bara det den är säker på. Inget flyttas till papperskorgen, och varje flytt går att ångra i historiken nedan. Dina egna regler under Inställningar används.',
  'Check every': 'Kontrollera var',
  'Saving…': 'Sparar…',
  Save: 'Spara',
  'Folders the AI may move to': 'Mappar som AI:n får flytta till',
  'The last check failed: {error}. It will be retried shortly.': 'Senaste kontrollen misslyckades: {error}. Ett nytt försök görs om en stund.',
  'On.': 'På.',
  'Last checked {date}.': 'Senast kontrollerad {date}.',
  'The first check will run shortly.': 'Första kontrollen görs strax.',
  History: 'Historik',
  'kept for {n} days': 'sparas i {n} dagar',
  'Nothing has been sorted automatically yet.': 'Inget har sorterats automatiskt ännu.',
  When: 'När',
  'Moved to': 'Flyttades till',
  Undo: 'Ångra',

  // Settings tab
  'The saved service ({url}) is no longer available on the server. Choose another one.':
    'Den sparade tjänsten ({url}) är inte längre tillgänglig på servern. Välj en annan.',
  'AI service': 'AI-tjänst',
  Model: 'Modell',
  'API key': 'API-nyckel',
  'Create a key': 'Skapa en nyckel',
  'Saved for this service – leave empty to keep it': 'Sparad för den här tjänsten – lämna tomt för att behålla',
  'Not needed for this service': 'Behövs inte för den här tjänsten',
  'Your own rules (optional)': 'Egna regler (valfritt)',
  'For example:\nReceipts and invoices go to Finance.\nNewsletters go to Read later.\nMail from the school stays in the inbox.':
    'Till exempel:\nKvitton och fakturor ska till Ekonomi.\nNyhetsbrev ska till Läs senare.\nMejl från skolan ska ligga kvar i inkorgen.',
  "The key is stored encrypted on the server, one per service, and is never shown again. If you switch service, the old service's key is kept until you choose Remove. When you sort, the sender, recipients, subject and the first {n} characters of each message are sent to the service you have chosen. Read the service's terms: on Gemini's free tier Google may use the content to improve its products, but not on the paid tier.":
    'Nyckeln sparas krypterad på servern, en per tjänst, och visas aldrig igen. Byter du tjänst finns den gamla tjänstens nyckel kvar tills du väljer Ta bort. När du sorterar skickas avsändare, mottagare, ämne och de första {n} tecknen av varje meddelande till tjänsten du har valt. Läs tjänstens villkor: på Geminis gratisnivå får Google använda innehållet för att förbättra sina produkter, men inte på betalnivån.',
  'The connection works. {n} models are available to choose from in the Model field.':
    'Anslutningen fungerar. {n} modeller finns att välja i fältet Modell.',
  Learned: 'Inlärt',
  'When you change the folder of a suggestion, decline it or undo an automatic move, it is saved as a correction. The AI gets the most relevant corrections as examples with every sort. Rules take precedence over the AI, also for automatic sorting and in the background.':
    'När du byter mapp på ett förslag, tackar nej till det eller ångrar en automatisk flytt sparas det som en rättning. AI:n får de mest närliggande rättningarna som exempel vid varje sortering. Regler gäller före AI:n, även vid automatisk sortering och i bakgrunden.',
  'Rules for incoming mail are managed under Rules.': 'Regler för inkommande post hanteras under Regler.',
  '{n} rules': '{n} regler',
  'Open Rules': 'Öppna Regler',
  Corrections: 'Rättningar',
  'Delete all': 'Ta bort alla',
  'No corrections yet.': 'Inga rättningar ännu.',
  'Delete correction': 'Ta bort rättningen',
  Remove: 'Ta bort',
  'Testing…': 'Testar…',
  Test: 'Testa',
}
