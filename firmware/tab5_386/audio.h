// Sound: the PC's speaker, Sound Blaster 16 and AdLib, mixed by tiny386 (mixer_callback, 44.1 kHz 16-bit stereo)
// and played through the Tab5's speaker (M5Unified: ES8388 codec and amplifier).
//
// A task on core 0 pulls the mix a block at a time. The mixer has to run at the sound's real pace even when nothing
// is heard: it is what drains the Sound Blaster's buffer, and programs wait for the interrupt that follows. So while
// the speaker plays, the speaker's queue sets the pace; while it is off, a timer does.
// The speaker (amplifier) is switched on when a block isn't silent and off again AUDIO_OFF_MS after the last sound:
// the amplifier alone (audio_amp). The I2S channel and the codec start once, at boot (audio_setup), before Wi-Fi:
// started with each sound, they took their DMA descriptors from DMA-capable internal RAM each time, of which Wi-Fi
// leaves a few hundred bytes now and then, and then failed ("i2s_alloc_dma_desc ... failed", "speaker: begin failed"
// every block): noise, at what sounded like the wrong pitch, instead of the sound.
// Only this task touches M5.Speaker after setup. Its begin/end/setVolume talk to the codec on M5Unified's internal
// I2C bus, which the touch screen (touch.h) shares: both hold g_i2c_lock. (The keyboard is on the other bus.)
#pragma once
#include <Preferences.h>

static constexpr int AUDIO_RATE = 44100;
static constexpr int AUDIO_CHUNK = 256;              // frames per mixer call (its buffer: MIXER_BUF_LEN samples)
static constexpr int AUDIO_BLOCK = 1024;             // frames per block handed to the speaker (23 ms)
static constexpr int AUDIO_BUFS = 3;                 // blocks in flight: playRaw() doesn't copy
static constexpr int AUDIO_CH = 0;
static constexpr int64_t AUDIO_OFF_MS = 5000;
static constexpr int AUDIO_QUIET = 64;               // |sample| at or below this counts as silence

static uint8_t g_volume = 160;                       // speaker volume 0-255 (Sym + Alt + Up/Down, serial vol <n>)
static Preferences g_prefs;                          // remembered: the volume
static SemaphoreHandle_t g_i2c_lock;                 // M5Unified's internal I2C bus: speaker codec, touch
struct AudioStats { uint32_t blocks, played, underruns, mix_max_us, peak; bool on; };   // mix_max_us, peak: since the last S
static AudioStats g_astats = {};
static int16_t* g_abuf[AUDIO_BUFS];
static bool g_spk_started = false;   // M5.Speaker began at boot (its I2S channel kept)

// The Tab5's amplifier (bit 1 of the PI4IOE5V6408 at 0x43, register 5, as M5Unified's Tab5 speaker callback); under
// g_i2c_lock
static void audio_amp(bool on) {
  if (on) M5.In_I2C.bitOn(0x43, 0x05, 0b00000010, 400000);
  else M5.In_I2C.bitOff(0x43, 0x05, 0b00000010, 400000);
}
static volatile bool g_audio_quiet = false;   // the PC paused or off (side_ui.h): no sound, not even held notes

static void audio_task(void*) {
  bool on = false;
  int next = 0;
  uint8_t vol = 0;
  int64_t last_sound = 0, due = esp_timer_get_time();
  const int64_t block_us = (int64_t)AUDIO_BLOCK * 1000000 / AUDIO_RATE;
  for (;;) {
    if (on) {   // paced by the speaker: keep two blocks queued
      while (M5.Speaker.isPlaying(AUDIO_CH) >= 2) vTaskDelay(1);
      if (!M5.Speaker.isPlaying(AUDIO_CH)) ++g_astats.underruns;
    } else {    // paced by the clock
      const int64_t wait = due - esp_timer_get_time();
      if (wait > 1000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
      due += block_us;
      if (esp_timer_get_time() - due > 4 * block_us) due = esp_timer_get_time();   // fell far behind: don't race
    }
    const int64_t m0 = esp_timer_get_time();
    int16_t* buf = g_abuf[next];
    memset(buf, 0, AUDIO_BLOCK * 4);
    if (!g_audio_quiet)
      for (int i = 0; i < AUDIO_BLOCK; i += AUDIO_CHUNK) mixer_callback(g_pc, (uint8_t*)(buf + 2 * i), AUDIO_CHUNK * 4);
    ++g_astats.blocks;
    g_astats.mix_max_us = max(g_astats.mix_max_us, (uint32_t)(esp_timer_get_time() - m0));
    bool loud = false;
    uint32_t peak = 0;   // (the loudest sample: serial S)
    for (int i = 0; i < AUDIO_BLOCK * 2; ++i) peak = max(peak, (uint32_t)abs(buf[i]));
    loud = peak > AUDIO_QUIET;
    g_astats.peak = max(g_astats.peak, peak);
    const int64_t now = esp_timer_get_time() / 1000;
    if (loud) last_sound = now;
    xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
    if (loud && !on && g_volume) {
      if (!g_spk_started) g_spk_started = M5.Speaker.begin();   // (at boot it failed: now)
      on = g_spk_started;
      vol = 0;
      if (on) audio_amp(true);
      else Serial.println("speaker: begin failed");
    }
    if (on && vol != g_volume) M5.Speaker.setVolume(vol = g_volume);
    if (on && (now - last_sound > AUDIO_OFF_MS || !g_volume)) {
      audio_amp(false);   // (the channel goes on, with nothing to play: silence)
      on = false;
      due = esp_timer_get_time();
    }
    xSemaphoreGive(g_i2c_lock);
    if (on) {
      M5.Speaker.playRaw(buf, AUDIO_BLOCK * 2, AUDIO_RATE, true, 1, AUDIO_CH);
      ++g_astats.played;
      next = (next + 1) % AUDIO_BUFS;
    }
    g_astats.on = on;
  }
}

static void set_volume(int v) {
  g_volume = (uint8_t)constrain(v, 0, 255);
  g_prefs.putUChar("vol", g_volume);
  Serial.printf("volume %u\n", g_volume);
}

static void audio_setup() {
  g_i2c_lock = xSemaphoreCreateMutex();
  g_prefs.begin("tab5_386", false);
  g_volume = g_prefs.getUChar("vol", 160);
  auto sc = M5.Speaker.config();
  sc.sample_rate = AUDIO_RATE;
  sc.stereo = true;
  sc.task_pinned_core = 0;   // core 1 is the PC's
  sc.task_priority = 5;
  M5.Speaker.config(sc);
  for (auto& b : g_abuf) b = (int16_t*)heap_caps_calloc(AUDIO_BLOCK * 2, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_spk_started = M5.Speaker.begin();   // (its DMA descriptors now, before Wi-Fi takes that RAM)
  if (g_spk_started) audio_amp(false);
  else Serial.println("speaker: begin failed");
}

static void audio_start() {   // once the PC exists
  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 4, nullptr, 0);
}

// Serial "spk restart": the speaker codec (ES8388) set up again, its I2S stream running on: powered down in order
// (muted, DAC and outputs off, chip off), then reset and configured as M5Unified's Tab5 speaker callback does. A
// codec left running by a restart of the Tab5 or a flash starts the next time from where it was, and once in a
// while all sound then crackled until the Tab5 was switched off. (Not M5.Speaker's end() and begin(): begin takes
// its DMA buffers anew, and with Wi-Fi on there is no such RAM left: no sound until the next start.)
static void audio_spk_restart() {
  static constexpr uint8_t ES = 0x10;
  static constexpr uint8_t setup[][2] = {   // (M5Unified.inl, the Tab5's _speaker_enabled_cb)
    {0, 0x80}, {0, 0x00}, {0, 0x00}, {0, 0x0E}, {1, 0x00}, {2, 0x0A}, {3, 0xFF}, {4, 0x3C}, {5, 0x00}, {6, 0x00},
    {7, 0x7C}, {8, 0x00}, {23, 0x18}, {24, 0x00}, {25, 0x20}, {26, 0x00}, {27, 0x00}, {28, 0x08}, {29, 0x00},
    {38, 0x00}, {39, 0xB8}, {42, 0xB8}, {43, 0x08}, {45, 0x00}, {46, 0x21}, {47, 0x21}, {48, 0x21}, {49, 0x21}};
  xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
  const bool amp = M5.In_I2C.readRegister8(0x43, 0x05, 400000) & 2;
  if (amp) audio_amp(false);
  bool ok = M5.In_I2C.writeRegister8(ES, 25, 0x24, 400000);   // mute (soft ramp)
  delay(1);
  ok = M5.In_I2C.writeRegister8(ES, 4, 0xC0, 400000) && ok;   // DAC and outputs off
  ok = M5.In_I2C.writeRegister8(ES, 2, 0xFF, 400000) && ok;   // chip off
  delay(5);
  for (auto& r : setup) ok = M5.In_I2C.writeRegister8(ES, r[0], r[1], 400000) && ok;
  // the state machines reset and started (CHIPPOWER: F0, then as set): at the start the codec is set up before its
  // clocks run, which starts them; set up while they run it stayed silent until this
  ok = M5.In_I2C.writeRegister8(ES, 2, 0xF0, 400000) && ok;
  delay(1);
  ok = M5.In_I2C.writeRegister8(ES, 2, 0x0A, 400000) && ok;
  if (amp) audio_amp(true);
  xSemaphoreGive(g_i2c_lock);
  Serial.printf("speaker: codec %s\n", ok ? "set up again" : "didn't answer on I2C");
}

// serial "codec": the ES8388's registers (0-52, as hex), and the amplifier's bit (PI4IOE5V6408 0x43 register 5 bit 1)
static void audio_codec_report() {
  uint8_t r[53] = {};
  xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
  bool ok = true;
  for (int i = 0; i < 53 && ok; ++i) ok = M5.In_I2C.readRegister(0x10, i, &r[i], 1, 400000);
  const uint8_t io = M5.In_I2C.readRegister8(0x43, 0x05, 400000);
  xSemaphoreGive(g_i2c_lock);
  if (!ok) { Serial.println("codec: no answer on I2C"); return; }
  for (int i = 0; i < 53; i += 16) {
    char line[80];
    int n = snprintf(line, sizeof line, "codec %02d:", i);
    for (int k = i; k < i + 16 && k < 53; ++k) n += snprintf(line + n, sizeof line - n, " %02x", r[k]);
    Serial.println(line);
  }
  Serial.printf("codec: amplifier %s, speaker %s, sound %s, volume %u\n", io & 2 ? "on" : "off",
                g_spk_started ? "started" : "not started", g_astats.on ? "playing" : "idle", g_volume);
}
