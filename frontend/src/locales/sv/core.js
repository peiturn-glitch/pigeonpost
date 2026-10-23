// Svenska översättningar för store.js, utils.js, richtext.js, zipreader.js och oauth.js.
// Nyckeln är den engelska texten exakt som den står i t()-anropet.
export default {
  // utils.js: storlek
  '{n} B': '{n} B',
  '{n} kB': '{n} kB',
  '{n} MB': '{n} MB',

  // utils.js: sorteringsetiketter (SORT_KEYS)
  'Date': 'Datum',
  'Sender': 'Avsändare',
  'Recipient': 'Mottagare',
  'Subject': 'Ämne',
  'Size': 'Storlek',

  // utils.js: specialmappar (specialFolderNames)
  'Inbox': 'Inkorg',
  'Drafts': 'Utkast',
  'Sent': 'Skickat',
  'Archive': 'Arkiv',
  'Junk': 'Skräppost',
  'Trash': 'Papperskorg',
  'All messages': 'Alla meddelanden',
  'Starred': 'Stjärnmärkt',
  'Important': 'Viktigt',

  // richtext.js
  'image': 'bild',
  'Unsupported image format. Use PNG, JPEG, GIF or WebP.': 'Bildformatet stöds inte. Använd PNG, JPEG, GIF eller WebP.',
  'The image could not be read': 'Bilden kunde inte läsas',

  // zipreader.js
  'The file is not a valid zip file.': 'Filen är ingen giltig zip-fil.',
  'The zip file uses ZIP64 but has no ZIP64 directory.': 'Zip-filen använder ZIP64 men saknar ZIP64-katalog.',
  'Invalid ZIP64 directory.': 'Ogiltig ZIP64-katalog.',
  'Corrupt zip file.': 'Skadad zip-fil.',
  'The zip file uses an unsupported compression method (method {method}).':
    'Zip-filen använder en komprimering som inte stöds (metod {method}).',
  'The zip file contains no prefs.js. Is it a Thunderbird export?': 'Zip-filen innehåller ingen prefs.js. Är det en Thunderbird-export?',

  // oauth.js
  'Sign-in was cancelled.': 'Inloggningen avbröts.',
  'The browser blocked the sign-in window. Allow pop-ups for this site and try again.':
    'Webbläsaren blockerade inloggningsfönstret. Tillåt popup-fönster för sidan och försök igen.',
  'Google sign-in failed.': 'Google-inloggningen misslyckades.',
  'Sign-in took too long. Please try again.': 'Inloggningen tog för lång tid. Försök igen.',

  // store.js: mappar
  'Name of the new folder:': 'Namn på den nya mappen:',
  'New name for the folder:': 'Nytt namn på mappen:',
  'A folder cannot be moved into itself': 'En mapp kan inte flyttas in i sig själv',
  'There is already a folder named "{name}" there': 'Det finns redan en mapp som heter "{name}" där',
  'the top level': 'översta nivån',
  'Move folder "{name}" to {target}?': 'Flytta mappen "{name}" till {target}?',
  'The mail server did not move the folder': 'E-postservern flyttade inte mappen',
  'Folder "{name}" was moved': 'Mappen "{name}" flyttades',
  'Delete folder "{name}" and all its contents? This cannot be undone.':
    'Ta bort mappen "{name}" och allt innehåll? Det går inte att ångra.',

  // store.js: meddelanden
  'Permanently delete 1 message?': 'Radera 1 meddelande permanent?',
  'Permanently delete {n} messages?': 'Radera {n} meddelanden permanent?',
  '1 message was moved': '1 meddelande flyttades',
  '{n} messages were moved': '{n} meddelanden flyttades',
  '1 message is in another account and was not moved. Moving between accounts is not supported yet.':
    '1 meddelande ligger i ett annat konto och flyttades inte. Flytt mellan konton stöds inte ännu.',
  '{n} messages are in another account and were not moved. Moving between accounts is not supported yet.':
    '{n} meddelanden ligger i ett annat konto och flyttades inte. Flytt mellan konton stöds inte ännu.',
  'The account has no junk folder': 'Kontot har ingen skräppostmapp',
  'The account has no archive folder': 'Kontot har ingen arkivmapp',

  // store.js: skriva
  'Add an account first': 'Lägg till ett konto först',
  'On {date}, {name} wrote:': 'Den {date} skrev {name}:',
  'Forwarded message': 'Vidarebefordrat meddelande',
  'From': 'Från',
  'To': 'Till',
  'Cc': 'Kopia',
}
