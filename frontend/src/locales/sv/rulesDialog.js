// Swedish translations for src/components/RulesDialog.vue (mail rules). Keys are the English strings used in t().
// Short keys shared with other files (Close, Cancel, Save, …) must have the same Swedish as there.
export default {
  // Dialog header, tabs and shared words
  Rules: 'Regler',
  Suggestions: 'Förslag',
  History: 'Historik',
  Close: 'Stäng',
  Cancel: 'Avbryt',
  Save: 'Spara',
  'Saving…': 'Sparar…',
  'Loading…': 'Läser in…',
  Account: 'Konto',
  Name: 'Namn',
  Edit: 'Redigera',
  Enabled: 'Aktiv',
  From: 'Från',
  To: 'Till',
  Subject: 'Ämne',
  When: 'När',
  Undo: 'Ångra',
  Undone: 'Ångrad',
  Done: 'Klar',
  Running: 'Pågår',
  Queued: 'I kö',
  Failed: 'Misslyckades',
  '(no subject)': '(inget ämne)',
  'Add an email account first.': 'Lägg till ett e-postkonto först.',

  // Status line
  'The last run failed: {error}': 'Senaste körningen misslyckades: {error}',
  'Last run {date}.': 'Senast körda {date}.',
  'The rules have not run yet.': 'Reglerna har inte körts ännu.',
  'The mail cache is off, so the preview and suggestions are unavailable.':
    'Mejlcachen är av, så förhandsvisningen och förslagen är inte tillgängliga.',

  // Rules list
  'Rules are applied on the server to new mail in the Inbox, in the order below, before the AI sorting. A rule that moves or deletes a message normally stops the rules after it.':
    'Reglerna tillämpas på servern på ny post i inkorgen, i ordningen nedan, före AI-sorteringen. En regel som flyttar eller tar bort ett meddelande stoppar normalt reglerna efter den.',
  'No rules yet. Create one, or let PigeonPost suggest rules from how you have sorted your mail so far.':
    'Inga regler ännu. Skapa en, eller låt PigeonPost föreslå regler utifrån hur du har sorterat din post hittills.',
  'The folder the rule moves to no longer exists.': 'Mappen som regeln flyttar till finns inte längre.',
  'Move up': 'Flytta upp',
  'Move down': 'Flytta ner',
  'Delete rule': 'Ta bort regeln',
  'Delete the rule "{name}"?': 'Ta bort regeln "{name}"?',
  'The rule was deleted': 'Regeln togs bort',
  'New rule': 'Ny regel',
  'At most {n} rules per account': 'Högst {n} regler per konto',
  'Run rules now': 'Kör reglerna nu',
  'Apply the enabled rules to everything in the Inbox now': 'Tillämpa de aktiva reglerna på allt i inkorgen nu',
  'Apply the enabled rules to all messages currently in the Inbox of {email}?':
    'Tillämpa de aktiva reglerna på alla meddelanden som just nu ligger i inkorgen för {email}?',
  'The rules run on the server in the background. Moves can be undone from the history.':
    'Reglerna körs på servern i bakgrunden. Flyttar går att ångra i historiken.',
  'Run in {folder}': 'Körning i {folder}',
  '{done} of {total} checked, {moved} moved, {changed} changed': '{done} av {total} kontrollerade, {moved} flyttade, {changed} ändrade',
  'Finished {date}.': 'Klar {date}.',

  // Rule summary
  '{field} contains {value}': '{field} innehåller {value}',
  '{field} is {value}': '{field} är {value}',
  '{field} starts with {value}': '{field} börjar med {value}',
  '{field} ends with {value}': '{field} slutar med {value}',
  '{field} has domain {value}': '{field} har domänen {value}',
  and: 'och',
  or: 'eller',
  'move to {folder}': 'flytta till {folder}',
  'mark as junk': 'markera som skräppost',
  delete: 'ta bort',
  'mark as read': 'markera som läst',
  star: 'stjärnmärk',
  'leave in place': 'ligger kvar',
  'leave in place, stop processing': 'ligger kvar, stoppa vidare regler',

  // Editor
  'Edit rule': 'Redigera regel',
  'Apply when': 'Gäller när',
  'all of the conditions match': 'alla villkor stämmer',
  'any of the conditions match': 'något av villkoren stämmer',
  contains: 'innehåller',
  is: 'är',
  'starts with': 'börjar med',
  'ends with': 'slutar med',
  'has domain': 'har domänen',
  'e.g. Invoice': 't.ex. Faktura',
  'Remove condition': 'Ta bort villkoret',
  'Add condition': 'Lägg till villkor',
  Then: 'Gör då',
  'Leave in place': 'Låt ligga kvar',
  'Move to folder': 'Flytta till mapp',
  'Choose a folder…': 'Välj mapp…',
  '{folder} (the folder no longer exists)': '{folder} (mappen finns inte längre)',
  'Mark as junk|rule': 'Markera som skräppost',
  'Delete (to trash)': 'Ta bort (till papperskorgen)',
  'Mark as read': 'Markera som läst',
  Star: 'Stjärnmärk',
  'Stop processing further rules': 'Stoppa vidare regler',
  'Fill in the conditions to see how many messages in the Inbox match.': 'Fyll i villkoren för att se hur många meddelanden i inkorgen som träffas.',
  'Checking the Inbox…': 'Kontrollerar inkorgen…',
  'Matches {n} of {total} messages in Inbox': 'Träffar {n} av {total} meddelanden i inkorgen',
  '(only part of the folder was checked; the real number may be higher)': '(bara en del av mappen kontrollerades; det riktiga antalet kan vara högre)',
  'Rule saved': 'Regeln sparades',

  // Suggestions
  'PigeonPost looks at the folders you have sorted mail into and suggests rules for senders that usually end up in the same place.':
    'PigeonPost tittar på mapparna du har sorterat post till och föreslår regler för avsändare som oftast hamnar på samma ställe.',
  'Suggest rules from my folders': 'Föreslå regler utifrån mina mappar',
  'Looking through your folders…': 'Går igenom dina mappar…',
  'Also ask the AI': 'Fråga även AI:n',
  'With the AI, sender addresses and subject lines per folder are sent to the AI service, never message bodies. This can take up to half a minute.':
    'Med AI:n skickas avsändaradresser och ämnesrader per mapp till AI-tjänsten, aldrig meddelandetexter. Det kan ta upp till en halv minut.',
  'Suggestions need the mail cache, which is off for this server.': 'Förslagen kräver mejlcachen, som är av på den här servern.',
  'No suggestions right now. Sort some mail into folders and try again later.':
    'Inga förslag just nu. Sortera lite post till mappar och försök igen senare.',
  AI: 'AI',
  'from your folders': 'från dina mappar',
  'Add rule': 'Lägg till regel',
  'No thanks': 'Nej tack',
  '{n} in {folder}, {m} elsewhere, {k} still in Inbox': '{n} i {folder}, {m} på annat håll, {k} kvar i inkorgen',
  'Some suggestions come from the AI. Check them before adding.': 'En del förslag kommer från AI:n. Kontrollera dem innan du lägger till.',

  // History
  'Messages the rules have moved. Undo moves a message back to where it was.': 'Meddelanden som reglerna har flyttat. Ångra flyttar tillbaka ett meddelande dit det låg.',
  'The rules have not moved anything yet.': 'Reglerna har inte flyttat något ännu.',
  Moved: 'Flyttat',
  Rule: 'Regel',
  'The message was moved back': 'Meddelandet flyttades tillbaka',
}
