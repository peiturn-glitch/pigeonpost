// Läser prefs.js ur en Thunderbird-export (zip) direkt i webbläsaren.
// Bara centralkatalogen och den aktuella filen läses, så även exporter på flera GB med e-post
// går snabbt och laddas aldrig upp till servern.

const EOCD = 0x06054b50
const ZIP64_LOCATOR = 0x07064b50
const ZIP64_EOCD = 0x06064b50
const CENTRAL_ENTRY = 0x02014b50
const LOCAL_HEADER = 0x04034b50

async function readBytes(file, offset, length) {
  const buffer = await file.slice(offset, offset + length).arrayBuffer()
  return new DataView(buffer)
}

function u64(view, offset) {
  return Number(view.getBigUint64(offset, true))
}

async function locateCentralDirectory(file) {
  const tailLength = Math.min(file.size, 22 + 65535)
  const tailStart = file.size - tailLength
  const tail = await readBytes(file, tailStart, tailLength)

  let eocd = -1
  for (let i = tailLength - 22; i >= 0; i--) {
    if (tail.getUint32(i, true) === EOCD) {
      eocd = i
      break
    }
  }
  if (eocd < 0) throw new Error('Filen är ingen giltig zip-fil.')

  let size = tail.getUint32(eocd + 12, true)
  let offset = tail.getUint32(eocd + 16, true)

  // ZIP64: stora exporter har 64-bitars offset i en separat post före EOCD.
  if (offset === 0xffffffff || size === 0xffffffff) {
    const locator = eocd - 20
    if (locator < 0 || tail.getUint32(locator, true) !== ZIP64_LOCATOR) {
      throw new Error('Zip-filen använder ZIP64 men saknar ZIP64-katalog.')
    }
    const zip64Offset = u64(tail, locator + 8)
    const record = await readBytes(file, zip64Offset, 56)
    if (record.getUint32(0, true) !== ZIP64_EOCD) throw new Error('Ogiltig ZIP64-katalog.')
    size = u64(record, 40)
    offset = u64(record, 48)
  }
  return { offset, size }
}

function* centralEntries(view) {
  const decoder = new TextDecoder()
  let pos = 0
  while (pos + 46 <= view.byteLength && view.getUint32(pos, true) === CENTRAL_ENTRY) {
    const method = view.getUint16(pos + 10, true)
    let compressedSize = view.getUint32(pos + 20, true)
    let uncompressedSize = view.getUint32(pos + 24, true)
    const nameLength = view.getUint16(pos + 28, true)
    const extraLength = view.getUint16(pos + 30, true)
    const commentLength = view.getUint16(pos + 32, true)
    let localOffset = view.getUint32(pos + 42, true)
    const name = decoder.decode(new Uint8Array(view.buffer, view.byteOffset + pos + 46, nameLength))

    // ZIP64-fältet (id 1) innehåller bara de värden som är 0xffffffff, i denna ordning.
    let extra = pos + 46 + nameLength
    const extraEnd = extra + extraLength
    while (extra + 4 <= extraEnd) {
      const id = view.getUint16(extra, true)
      const length = view.getUint16(extra + 2, true)
      if (id === 1) {
        let p = extra + 4
        if (uncompressedSize === 0xffffffff) { uncompressedSize = u64(view, p); p += 8 }
        if (compressedSize === 0xffffffff) { compressedSize = u64(view, p); p += 8 }
        if (localOffset === 0xffffffff) { localOffset = u64(view, p) }
      }
      extra += 4 + length
    }

    yield { name, method, compressedSize, uncompressedSize, localOffset }
    pos += 46 + nameLength + extraLength + commentLength
  }
}

async function readEntry(file, entry) {
  const header = await readBytes(file, entry.localOffset, 30)
  if (header.getUint32(0, true) !== LOCAL_HEADER) throw new Error('Skadad zip-fil.')
  const dataStart = entry.localOffset + 30 + header.getUint16(26, true) + header.getUint16(28, true)
  const blob = file.slice(dataStart, dataStart + entry.compressedSize)

  if (entry.method === 0) return new TextDecoder().decode(await blob.arrayBuffer())
  if (entry.method === 8) {
    const stream = blob.stream().pipeThrough(new DecompressionStream('deflate-raw'))
    return new TextDecoder().decode(await new Response(stream).arrayBuffer())
  }
  throw new Error(`Zip-filen använder en komprimering som inte stöds (metod ${entry.method}).`)
}

// Returnerar innehållet i prefs.js, antingen i zip-filens rot eller i en enda toppmapp
// (samma regel som Thunderbirds egen import).
export async function readPrefsFromZip(file) {
  const { offset, size } = await locateCentralDirectory(file)
  const directory = await readBytes(file, offset, size)
  let best = null
  for (const entry of centralEntries(directory)) {
    if (entry.name === 'prefs.js' || /^[^/]+\/prefs\.js$/.test(entry.name)) {
      if (!best || entry.name.length < best.name.length) best = entry
    }
  }
  if (!best) throw new Error('Zip-filen innehåller ingen prefs.js. Är det en Thunderbird-export?')
  return readEntry(file, best)
}
