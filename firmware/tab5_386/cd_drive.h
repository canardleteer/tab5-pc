// The PC's CD-ROM drive. Every machine has one: the first CD-ROM line of its ini (cda-cdd), or, without one, an empty
// drive in the first free place of the second IDE channel (cdc, else cdd). An image that isn't on the card leaves the
// drive empty (not gone). The side panel's tray (side_ui.h) and the settings panel's dialog (settings_ui.h) change the
// disc while the PC runs (vendor/patches/0021: the guest is told the medium changed, as a real drive does), and the
// disc is written into the machine's ini, so it is still in the drive at the next start, as on a real PC.
#pragma once

static int g_cd_drive = -1;          // its place, g_conf.disks' index (-1: none, all four taken by hard disks)
static char g_cd_path[2][192];       // the disc's image ("": none); g_conf.disks[g_cd_drive] points at one of them,
static int g_cd_buf = 0;             // the other one written on a change (the web server reads it: pc_uses)
static std::atomic<uint32_t> g_cd_seq{0};   // counts changes (the side panel's tray follows)

// From load_config, after the disks not on the card were left out (a CD image's left in, as ""): the drive's place
static void cd_setup(PCConfig& conf) {
  g_cd_drive = -1;
  for (int i = 0; i < 4 && g_cd_drive < 0; ++i)
    if (conf.iscd[i] && conf.disks[i]) g_cd_drive = i;
  for (int i : {2, 3})
    if (g_cd_drive < 0 && !conf.disks[i]) {
      conf.iscd[i] = 1;
      g_cd_drive = i;
    }
  if (g_cd_drive < 0) {
    Serial.println("cd: no place for a CD-ROM drive (four hard disks)");
    return;
  }
  snprintf(g_cd_path[0], sizeof(g_cd_path[0]), "%s", conf.disks[g_cd_drive] ? conf.disks[g_cd_drive] : "");
  conf.disks[g_cd_drive] = g_cd_path[0];
  Serial.printf("cd: drive cd%c, %s\n", 'a' + g_cd_drive, g_cd_path[0][0] ? g_cd_path[0] : "no disc");
}

static const char* cd_disc() { return g_cd_drive < 0 ? "" : g_cd_path[g_cd_buf]; }

// The disc's file name without its folder ("" if none)
static String cd_disc_name() {
  const char* p = cd_disc();
  const char* slash = strrchr(p, '/');
  return slash ? slash + 1 : p;
}

// The machine's ini with "<key> = <value>" in its [pc] section: the line replaced, or added after "[pc]". Written
// beside it, then put in its place (a Tab5 reset halfway leaves the old one, or the new one under .new).
static bool ini_set_pc_key(const String& path, const char* key, const char* value) {
  String text;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[256];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.concat(buf, n);
    fclose(f);
  }
  const char* eol = text.indexOf("\r\n") >= 0 || text.isEmpty() ? "\r\n" : "\n";
  const String set = String(key) + " = " + value;
  String out, section;
  bool done = false;
  for (int at = 0; at < (int)text.length();) {
    const int nl = text.indexOf('\n', at), end = nl < 0 ? text.length() : nl + 1;
    String line = text.substring(at, end), l = line;
    at = end;
    l.trim();
    l.toLowerCase();
    if (l.startsWith("[")) {
      if (section == "[pc]" && !done) out += set + eol, done = true;   // (a [pc] without it: at its end)
      section = l;
    } else if (section == "[pc]" && !done && l.startsWith(key)) {
      String rest = l.substring(strlen(key));
      rest.trim();
      if (rest.startsWith("=")) {
        line = set + (line.endsWith("\r\n") ? "\r\n" : line.endsWith("\n") ? "\n" : eol);
        done = true;
      }
    }
    out += line;
  }
  if (!done) {
    if (section == "[pc]") {
      if (out.length() && !out.endsWith("\n")) out += eol;
      out += set + eol;
    } else {
      out += String(out.length() && !out.endsWith("\n") ? eol : "") + "[pc]" + eol + set + eol;
    }
  }
  const String tmp = path + ".new";
  if (!fd_write_file(tmp.c_str(), out.c_str())) return false;
  unlink(path.c_str());
  return rename(tmp.c_str(), path.c_str()) == 0;
}

// A disc into the drive (path "": the drive emptied), as from the tray. The PC is held for the moment (its IDE state
// changes; the disc's image is opened on the card). False: no drive, or the image doesn't open (the old disc stays).
static bool cd_change(const char* path) {
  if (g_cd_drive < 0 || !g_pc || g_pc_stopped) return false;
  char* next = g_cd_path[g_cd_buf ^ 1];
  snprintf(next, sizeof(g_cd_path[0]), "%s", path);
  pc_hold(true);
  const bool ok = ide_change_cd(g_cd_drive < 2 ? g_pc->ide : g_pc->ide2, g_cd_drive & 1, next) == 0;
  if (ok) {
    g_cd_buf ^= 1;
    g_conf.disks[g_cd_drive] = next;
  }
  pc_hold(false);
  if (!ok) {
    Serial.printf("cd: %s didn't open; the disc in the drive stays\n", path);
    return false;
  }
  ++g_cd_seq;
  char key[4] = {'c', 'd', (char)('a' + g_cd_drive), 0};
  const String ini = machine_path();
  const bool saved = file_exists(ini.c_str()) && ini_set_pc_key(ini, key, next);
  Serial.printf("cd: %s%s\n", next[0] ? next : "no disc", saved ? "" : " (not saved in the machine's ini)");
  return true;
}
