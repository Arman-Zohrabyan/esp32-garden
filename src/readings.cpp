#include "readings.h"
#include "config.h"
#include <Preferences.h>

// The buffer is kept entirely in RAM and synced with NVS as a single block.
//
// Why a single block and not a key per record: Preferences stores each
// key as a separate entry in the NVS partition, and 30 keys would eat a noticeable part
// of the 20 KB partition on bookkeeping structures. One putBytes is also
// one erase operation instead of thirty.
// All buffer state lives in ONE struct and is written with a single putBytes.
//
// Why not three keys (data, count, head): each putBytes/putInt is
// a separate operation in the flash partition, and three calls would cost three times
// as much endurance as one. At 100,000 cycles per sector it's the difference between
// 11 and 4 years of life.
struct BufferBlob {
  uint32_t      magic;      // guards against reading a foreign/corrupt block
  int32_t       count;      // how many records are occupied
  int32_t       head;       // index of the oldest record
  uint32_t      writes;     // total saves to flash over the board's lifetime
  StoredReading items[READINGS_CAPACITY];
};

// Changes together with the BufferBlob format: on a mismatch the buffer is considered
// invalid and reset, rather than read as garbage.
#define BUFFER_MAGIC 0x7B0F1A03u

static BufferBlob blob;
static bool dirty = false;  // there are unsaved changes

// Preferences is local to each function, not file-static: with a
// shared object a nested begin() silently keeps the PREVIOUS
// namespace (details in the comment in state.cpp).

static const char* NS      = "rdbuf";
static const char* KEY_BUF = "blob";

// Saves the buffer to NVS if it changed. Called from within push/drop —
// no need to call it from outside.
//
// We write right after the change, not "sometime later": the board goes into
// deep sleep straight from the active cycle, and a deferred write simply wouldn't
// happen — RAM is lost on going to sleep.
static void persist() {
  if (!dirty) return;

  // Increment the counter BEFORE writing so the new value lands in the same
  // putBytes. No separate operation for it — otherwise wear tracking
  // would itself become a source of wear, doubling it at that.
  blob.writes++;

  Preferences prefs;
  size_t written = 0;
  if (prefs.begin(NS, false)) {
    written = prefs.putBytes(KEY_BUF, &blob, sizeof(blob));  // one operation for everything
    prefs.end();
  }

  // The write failed (partition full or corrupted) — we can't stay silent:
  // the board goes into deep sleep right after persist, and the unsaved reading
  // would vanish without a trace. dirty stays set: the data is alive in RAM,
  // and the next push in this same awake period will retry. The counter
  // is rolled back — no write happened, nothing to count.
  if (written != sizeof(blob)) {
    blob.writes--;
    Serial.println("ОШИБКА: буфер замеров не записался в NVS!");
    return;
  }

  dirty = false;
}

void readingsBegin() {
  bool readable = false;   // the block was read with the right size
  bool valid    = false;   // and magic matched

  Preferences prefs;
  prefs.begin(NS, true);                     // true = read-only
  // The size must match exactly. If it doesn't, the buffer either was never created,
  // or is left over from firmware with a different READINGS_CAPACITY. In the latter case
  // it must not be read: record boundaries would be shifted, and garbage
  // with foreign bootCounts would be sent.
  if (prefs.getBytesLength(KEY_BUF) == sizeof(blob)) {
    prefs.getBytes(KEY_BUF, &blob, sizeof(blob));
    readable = true;
    valid = (blob.magic == BUFFER_MAGIC);
  }
  prefs.end();

  // Guard against corrupt values: with garbage in count the send loop would run past
  // the array bounds.
  if (!valid ||
      blob.count < 0 || blob.count > READINGS_CAPACITY ||
      blob.head  < 0 || blob.head  >= READINGS_CAPACITY) {
    // Keep the write counter if the block was at least readable: a change of the
    // block format (new magic with the same size) doesn't reset the wear history —
    // it's the same sector after all. With a mismatched magic the value may also be
    // garbage, so a clearly impossible one (more than the flash endurance)
    // is discarded.
    uint32_t keepWrites = readable ? blob.writes : 0;
    if (!valid && keepWrites > NVS_ENDURANCE) keepWrites = 0;
    blob.magic  = BUFFER_MAGIC;
    blob.count  = 0;
    blob.head   = 0;
    blob.writes = keepWrites;
    // dirty is NOT set: no point writing an empty buffer to flash, it will
    // read as empty on the next load anyway.
  } else if (blob.count > 0) {
    Serial.printf("Буфер замеров: %d из %d (не отправлено)\n",
                  blob.count, READINGS_CAPACITY);
  }

  Serial.printf("Записей в NVS за жизнь платы: %lu (~%d%% от %lu)\n",
                (unsigned long)blob.writes, readingsWearPercent(),
                NVS_ENDURANCE);
}

uint32_t readingsWriteCount() { return blob.writes; }

int readingsWearPercent() {
  // NVS_ENDURANCE is the rated endurance of a flash sector. Real wear is lower:
  // NVS spreads writes across the sector (wear leveling), so dividing
  // the counter directly by endurance is an upper-bound estimate, not an exact number.
  return (int)((blob.writes * 100ULL) / NVS_ENDURANCE);
}

void readingsPush(const SoilReadingPayload& r) {
  int slot;

  if (blob.count < READINGS_CAPACITY) {
    // There's room — put it right after the last record.
    slot = (blob.head + blob.count) % READINGS_CAPACITY;
    blob.count++;
  } else {
    // Buffer full: evict the oldest reading. Fresh data is more valuable —
    // day-old moisture readings no longer decide anything.
    slot = blob.head;
    blob.head = (blob.head + 1) % READINGS_CAPACITY;
    Serial.println("Буфер полон — вытесняю самый старый замер");
  }

  StoredReading& s = blob.items[slot];
  s.raw       = (int16_t)r.raw;
  s.percent   = (int16_t)r.percent;
  s.minRaw    = (int16_t)r.minRaw;
  s.maxRaw    = (int16_t)r.maxRaw;
  s.rssi      = (int16_t)r.rssi;
  s.bootCount = r.bootCount;
  s.uptimeSec = r.uptimeSec;

  dirty = true;
  persist();

  Serial.printf("Замер отложен в буфер: %d из %d\n",
                blob.count, READINGS_CAPACITY);
}

int  readingsCount() { return blob.count; }
bool readingsEmpty() { return blob.count == 0; }
bool readingsFull()  { return blob.count >= READINGS_CAPACITY; }

int readingsFillBatch(ReportBatch& batch) {
  int n = blob.count < READINGS_SEND_CHUNK ? blob.count : READINGS_SEND_CHUNK;

  for (int i = 0; i < n; i++) {
    const StoredReading& s = blob.items[(blob.head + i) % READINGS_CAPACITY];

    SoilReadingPayload r;
    r.raw       = s.raw;
    r.percent   = s.percent;
    r.minRaw    = s.minRaw;
    r.maxRaw    = s.maxRaw;
    r.rssi      = s.rssi;
    r.bootCount = s.bootCount;
    r.uptimeSec = s.uptimeSec;

    // Voltage isn't stored: it's computed unambiguously from raw, and keeping it
    // in NVS would mean spending 4 bytes per record for the sake of arithmetic.
    r.voltage = s.raw * 3.3f / 4095.0f;

    batch.addReading(r);
  }

  return n;
}

void readingsDropSent(int n) {
  if (n <= 0) return;
  if (n > blob.count) n = blob.count;

  blob.head   = (blob.head + n) % READINGS_CAPACITY;
  blob.count -= n;
  dirty = true;
  persist();

  Serial.printf("Отправлено и удалено из буфера: %d, осталось %d\n",
                n, blob.count);
}

void readingsClear() {
  if (blob.count == 0) return;     // nothing to clear — don't touch flash
  blob.count = 0;
  blob.head  = 0;
  dirty = true;
  persist();
  Serial.println("Буфер замеров очищен");
}
