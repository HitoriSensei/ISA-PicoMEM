// Minimal HTTP server for the SD card, using the lwIP raw TCP API.
//   GET    /                   browser page
//   GET    /mac                Wifi MAC address
//   GET    /ls?p=DIR           directory listing, one "d|f <TAB> size <TAB> name" line per entry
//   GET    /f?p=FILE           download
//   PUT    /f?p=FILE           upload
//   DELETE /f?p=PATH           delete a file or an empty directory
//   POST   /d?p=DIR            create a directory
//   POST   /mv?p=PATH&t=PATH   rename or move

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#include "ff.h"
#include "dev_ne2000.h"     // PM_Wifi (MAC address)
#include "../pm_debug.h"
#include "pm_netif.h"
#include "pm_httpd.h"
#include "pm_httpd_page.h"

bool PM_PICO_EnablePSRAM();
bool PM_EnableSD();

#define HTTP_PORT     80
#define MAX_CONN      3         // Browsers open speculative connections
#define HDR_MAX       640
#define CHUNK         1024
#define PATH_MAX_LEN  256
#define HDR_IDLE_MAX  5         // Poll periods without a complete request (2s each)
#define IDLE_MAX      30        // Poll periods without progress (2s each)

typedef enum { C_HDR, C_BODY, C_SEND_PAGE, C_SEND_LS, C_SEND_FILE, C_CLOSING } cstate_t;

typedef struct {
  struct tcp_pcb *pcb;          // NULL for a free slot
  cstate_t st;
  uint16_t hlen;
  char hdr[HDR_MAX];
  uint32_t remain;              // Body bytes of a PUT still to receive
  size_t pos;                   // Page position
  bool fil_open, dir_open;
  FIL fil;
  DIR dir;
  uint8_t idle;
} conn_t;

static struct {
  bool enabled;
  ip4_addr_t ip, mask, gw;
  char key[40];
} cfg;

static conn_t conns[MAX_CONN];
static FILINFO fno;             // Shared buffer used only within a single call
static uint8_t chunk[CHUNK];    // Shared buffer used only within a single call

static inline void sd_on(void)  { PM_EnableSD(); }
static inline void sd_off(void) { PM_PICO_EnablePSRAM(); }

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Gets the URL decoded value of a query parameter
static bool qget(const char *q, const char *key, char *out, size_t size)
{
  size_t kl = strlen(key);
  while (q && *q) {
      if (!strncmp(q, key, kl) && q[kl] == '=') {
          const char *v = q + kl + 1;
          size_t n = 0;
          while (*v && *v != '&') {
              char c = *v++;
              if (c == '%' && hexval(v[0]) >= 0 && hexval(v[1]) >= 0) {
                  c = (hexval(v[0]) << 4) | hexval(v[1]);
                  v += 2;
                 }
              if (n + 1 >= size) return false;
              out[n++] = c;
             }
          out[n] = 0;
          return true;
         }
      q = strchr(q, '&');
      if (q) q++;
     }
  return false;
}

// Builds the absolute normalized path of the arg
static bool resolve(char *out, const char *arg)
{
  char tmp[PATH_MAX_LEN + 2];
  snprintf(tmp, sizeof(tmp), "/%s", arg);
  size_t len = 0;
  out[0] = 0;
  char *save;
  for (char *c = strtok_r(tmp, "/", &save); c != NULL; c = strtok_r(NULL, "/", &save)) {
      if (strcmp(c, ".") == 0) continue;
      if (strcmp(c, "..") == 0) {          // ".." stops at the root
          char *slash = strrchr(out, '/');
          if (slash) { *slash = 0; len = slash - out; }
          continue;
         }
      size_t cl = strlen(c);
      if (len + 1 + cl >= PATH_MAX_LEN) return false;
      out[len++] = '/';
      memcpy(out + len, c, cl);
      len += cl;
      out[len] = 0;
     }
  if (len == 0) { out[0] = '/'; out[1] = 0; }
  return true;
}

static bool put(conn_t *c, const void *d, size_t n)
{
  return tcp_write(c->pcb, d, n, TCP_WRITE_FLAG_COPY) == ERR_OK;
}

static void status(conn_t *c, int code, const char *text)
{
  char b[100];
  int n = snprintf(b, sizeof(b), "HTTP/1.0 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, text);
  put(c, b, n);
  c->st = C_CLOSING;
}

static void header(conn_t *c, const char *type, long long len)
{
  char b[160];
  int n = snprintf(b, sizeof(b), "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n", type);
  if (len >= 0) n += snprintf(b + n, sizeof(b) - n, "Content-Length: %lld\r\n", len);
  n += snprintf(b + n, sizeof(b) - n, "\r\n");
  put(c, b, n);
}

// ---------------------------------------------------------------------------
//  Connections
// ---------------------------------------------------------------------------

// Closes the files and the connection. Returns true if the connection was aborted.
static bool conn_close(conn_t *c, bool abort)
{
  if (c->fil_open) { sd_on(); f_close(&c->fil);    sd_off(); c->fil_open = false; }
  if (c->dir_open) { sd_on(); f_closedir(&c->dir); sd_off(); c->dir_open = false; }
  bool aborted = false;
  if (c->pcb) {
      struct tcp_pcb *pcb = c->pcb;
      c->pcb = NULL;
      tcp_arg(pcb, NULL);
      tcp_recv(pcb, NULL);
      tcp_sent(pcb, NULL);
      tcp_poll(pcb, NULL, 0);
      tcp_err(pcb, NULL);
      if (abort || tcp_close(pcb) != ERR_OK) { tcp_abort(pcb); aborted = true; }
     }
  return aborted;
}

static void handle_request(conn_t *c)
{
  // Reads the Content-Length before the request line is split
  bool have_cl = false;
  uint32_t clen = 0;
  for (char *l = strstr(c->hdr, "\r\n"); l != NULL; l = strstr(l + 2, "\r\n")) {
      if (!strncasecmp(l + 2, "content-length:", 15)) { clen = strtoul(l + 17, NULL, 10); have_cl = true; }
     }

  char *method = c->hdr;
  char *target = strchr(method, ' ');
  if (!target) { status(c, 400, "Bad Request"); return; }
  *target++ = 0;
  char *end = strchr(target, ' ');
  if (end) *end = 0;
  char *query = strchr(target, '?');
  if (query) *query++ = 0;

  if (!strcmp(method, "GET") && !strcmp(target, "/")) {
      header(c, "text/html; charset=utf-8", sizeof(http_page) - 1);
      c->pos = 0;
      c->st = C_SEND_PAGE;
      return;
     }

  if (!strcmp(method, "GET") && !strcmp(target, "/mac")) {   // Needs no password because the MAC is visible on the network anyway
      const uint8_t *m = PM_Wifi.cyw43_mac;
      char b[18];
      int n = snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
      header(c, "text/plain", n);
      put(c, b, n);
      c->st = C_CLOSING;
      return;
     }

  char key[sizeof(cfg.key) + 1], raw[PATH_MAX_LEN], path[PATH_MAX_LEN], fp[PATH_MAX_LEN + 4];
  // The password comes in the k query parameter
  if (cfg.key[0] && !(qget(query, "k", key, sizeof(key)) && !strcmp(key, cfg.key))) {
      status(c, 401, "Unauthorized");
      return;
     }
  if (!qget(query, "p", raw, sizeof(raw))) strcpy(raw, "/");
  if (!resolve(path, raw)) { status(c, 400, "Bad Request"); return; }
  snprintf(fp, sizeof(fp), "0:%s", path);
  bool root = (path[1] == 0);

  if (!strcmp(method, "GET") && !strcmp(target, "/ls")) {
      sd_on();
      FRESULT fr = f_opendir(&c->dir, fp);
      sd_off();
      if (fr != FR_OK) { status(c, 404, "Not Found"); return; }
      c->dir_open = true;
      header(c, "text/plain; charset=utf-8", -1);
      c->st = C_SEND_LS;
     }
  else if (!strcmp(method, "GET") && !strcmp(target, "/f")) {
      sd_on();
      FRESULT fr = root ? FR_INVALID_NAME : f_stat(fp, &fno);
      if (fr == FR_OK && !(fno.fattrib & AM_DIR)) fr = f_open(&c->fil, fp, FA_READ | FA_OPEN_EXISTING);
      else fr = FR_NO_FILE;
      sd_off();
      if (fr != FR_OK) { status(c, 404, "Not Found"); return; }
      c->fil_open = true;
      header(c, "application/octet-stream", (long long)f_size(&c->fil));
      c->st = C_SEND_FILE;
     }
  else if (!strcmp(method, "PUT") && !strcmp(target, "/f")) {
      if (root || !have_cl) { status(c, root ? 400 : 411, root ? "Bad Request" : "Length Required"); return; }
      sd_on();
      FRESULT fr = f_open(&c->fil, fp, FA_WRITE | FA_CREATE_ALWAYS);
      sd_off();
      if (fr != FR_OK) { status(c, 500, "Can't create file"); return; }
      c->fil_open = true;
      c->remain = clen;
      if (clen == 0) {
          sd_on(); fr = f_close(&c->fil); sd_off();
          c->fil_open = false;
          if (fr == FR_OK) status(c, 200, "OK"); else status(c, 500, "Write error");
         }
      else c->st = C_BODY;
     }
  else if (!strcmp(method, "DELETE") && !strcmp(target, "/f")) {
      sd_on();
      FRESULT fr = root ? FR_DENIED : f_unlink(fp);
      sd_off();
      if (fr == FR_OK) status(c, 200, "OK"); else status(c, 500, "Delete failed");
     }
  else if (!strcmp(method, "POST") && !strcmp(target, "/d")) {
      sd_on();
      FRESULT fr = root ? FR_EXIST : f_mkdir(fp);
      sd_off();
      if (fr == FR_OK) status(c, 200, "OK"); else status(c, 500, "Mkdir failed");
     }
  else if (!strcmp(method, "POST") && !strcmp(target, "/mv")) {   // Renames or moves p to t
      char to[PATH_MAX_LEN + 4];
      if (root || !qget(query, "t", raw, sizeof(raw)) || !resolve(path, raw) || path[1] == 0) { status(c, 400, "Bad Request"); return; }
      snprintf(to, sizeof(to), "0:%s", path);
      sd_on();
      FRESULT fr = f_rename(fp, to);
      sd_off();
      if (fr == FR_OK) status(c, 200, "OK");
         else if (fr == FR_EXIST) status(c, 409, "Already exists");
         else status(c, 500, "Rename failed");
     }
  else status(c, 404, "Not Found");
}

static err_t conn_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
  conn_t *c = (conn_t *)arg;

  if (p == NULL) {   // The client closed its side. This only matters before the answer is sent.
      if (c->st == C_HDR || c->st == C_BODY) return conn_close(c, true) ? ERR_ABRT : ERR_OK;
      return ERR_OK;
     }

  tcp_recved(pcb, p->tot_len);
  c->idle = 0;

  for (struct pbuf *q = p; q != NULL && c->st != C_CLOSING; q = q->next) {
      const uint8_t *d = (const uint8_t *)q->payload;
      u16_t i = 0;
      while (i < q->len && c->st != C_CLOSING) {
          if (c->st == C_HDR) {
              char ch = d[i++];
              if (c->hlen >= HDR_MAX - 1) { status(c, 400, "Bad Request"); break; }
              c->hdr[c->hlen++] = ch;
              if (ch == '\n' && c->hlen >= 4 && !memcmp(c->hdr + c->hlen - 4, "\r\n\r\n", 4)) {
                  c->hdr[c->hlen] = 0;
                  handle_request(c);
                 }
             }
          else if (c->st == C_BODY) {
              uint32_t n = q->len - i;
              if (n > c->remain) n = c->remain;
              UINT bw = 0;
              sd_on();
              FRESULT fr = f_write(&c->fil, d + i, n, &bw);
              sd_off();
              if (fr != FR_OK || bw != n) { status(c, 500, "Write error (SD full ?)"); break; }
              i += n;
              c->remain -= n;
              if (c->remain == 0) {
                  sd_on(); fr = f_close(&c->fil); sd_off();
                  c->fil_open = false;
                  if (fr == FR_OK) status(c, 200, "OK"); else status(c, 500, "Write error");
                 }
             }
          else break;   // Anything else is ignored
         }
     }
  pbuf_free(p);

  if (c->pcb) tcp_output(pcb);
  if (c->st == C_CLOSING) return conn_close(c, false) ? ERR_ABRT : ERR_OK;
  return ERR_OK;
}

static err_t conn_poll(void *arg, struct tcp_pcb *pcb)
{
  conn_t *c = (conn_t *)arg;
  if (++c->idle >= (c->st == C_HDR ? HDR_IDLE_MAX : IDLE_MAX)) return conn_close(c, true) ? ERR_ABRT : ERR_OK;
  return ERR_OK;
}

static void conn_err(void *arg, err_t err)
{
  conn_t *c = (conn_t *)arg;
  c->pcb = NULL;              // Already freed by lwIP
  conn_close(c, true);        // Only closes the files
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

  conn_t *c = NULL;
  for (int i = 0; i < MAX_CONN; i++) if (conns[i].pcb == NULL) { c = &conns[i]; break; }
  if (c == NULL) { tcp_abort(newpcb); return ERR_ABRT; }

  memset(c, 0, sizeof(*c));
  c->pcb = newpcb;
  c->st = C_HDR;
  tcp_arg(newpcb, c);
  tcp_recv(newpcb, conn_recv);
  tcp_err(newpcb, conn_err);
  tcp_poll(newpcb, conn_poll, 4);      // Every 2s
  return ERR_OK;
}

// Sends the next part of the answer
static void serve(conn_t *c)
{
  struct tcp_pcb *pcb = c->pcb;
  if (tcp_sndqueuelen(pcb) > TCP_SND_QUEUELEN - 3) return;

  switch (c->st) {
    case C_SEND_PAGE: {
        size_t n = sizeof(http_page) - 1 - c->pos;
        if (n > CHUNK) n = CHUNK;
        if (n > tcp_sndbuf(pcb)) n = tcp_sndbuf(pcb);
        if (n == 0) return;
        if (!put(c, http_page + c->pos, n)) return;
        c->pos += n;
        tcp_output(pcb);
        c->idle = 0;
        if (c->pos >= sizeof(http_page) - 1) conn_close(c, false);
        break;
       }
    case C_SEND_FILE: {
        if (tcp_sndbuf(pcb) < CHUNK) return;
        UINT br = 0;
        sd_on();
        FRESULT fr = f_read(&c->fil, chunk, CHUNK, &br);
        sd_off();
        if (fr != FR_OK) { conn_close(c, true); return; }   // The client sees the cut because the length is known
        if (br == 0) { conn_close(c, false); return; }
        if (!put(c, chunk, br)) {
            sd_on(); f_lseek(&c->fil, f_tell(&c->fil) - br); sd_off();   // Read it again later
            return;
           }
        tcp_output(pcb);
        c->idle = 0;
        break;
       }
    case C_SEND_LS: {
        if (tcp_sndbuf(pcb) < 320) return;
        sd_on();
        FRESULT fr = f_readdir(&c->dir, &fno);
        sd_off();
        if (fr != FR_OK || fno.fname[0] == 0) { conn_close(c, false); return; }
        int n = snprintf((char *)chunk, 320, "%c\t%llu\t%s\n", (fno.fattrib & AM_DIR) ? 'd' : 'f',
                         (unsigned long long)fno.fsize, fno.fname);
        if (n > 319) n = 319;
        put(c, chunk, n);
        tcp_output(pcb);
        c->idle = 0;
        break;
       }
    default: break;
   }
}

// ---------------------------------------------------------------------------
//  Init / task
// ---------------------------------------------------------------------------

static void trim(char *s)
{
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

// Reads the next value line. Empty lines and lines starting with # are skipped.
static bool cfg_line(FIL *f, char *out, int size)
{
  while (f_gets(out, size, f) != NULL) {
      trim(out);
      if (out[0] != 0 && out[0] != '#') return true;
     }
  return false;
}

bool pm_httpd_init(void)
{
  FIL f;
  char l[64];
  bool ok = false;

  memset(&cfg, 0, sizeof(cfg));

  if (!PM_EnableSD()) { PM_PICO_EnablePSRAM(); return false; }
  // The http.txt holds the IP, the mask, the optional gateway and the optional password, one per line
  FRESULT fr = f_open(&f, "http.txt", FA_OPEN_EXISTING | FA_READ);
  if (fr == FR_OK) {
      ok = cfg_line(&f, l, sizeof(l)) && ip4addr_aton(l, &cfg.ip) &&
           cfg_line(&f, l, sizeof(l)) && ip4addr_aton(l, &cfg.mask);
      if (ok) {
          if (!(cfg_line(&f, l, sizeof(l)) && ip4addr_aton(l, &cfg.gw))) ip4_addr_set_zero(&cfg.gw);
          if (!cfg_line(&f, cfg.key, sizeof(cfg.key))) cfg.key[0] = 0;
         }
      f_close(&f);
     }
  PM_PICO_EnablePSRAM();

  if (!ok) {
      PM_INFO("HTTP: disabled (no valid http.txt)\n");
      return false;
     }

  if (!pm_netif_init(PM_Wifi.cyw43_mac, &cfg.ip, &cfg.mask, &cfg.gw)) return false;

  struct tcp_pcb *pcb = tcp_new();
  if (pcb == NULL || tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT) != ERR_OK) return false;
  struct tcp_pcb *lpcb = tcp_listen(pcb);
  if (lpcb == NULL) return false;
  tcp_accept(lpcb, http_accept);

  cfg.enabled = true;
  PM_INFO("HTTP: listening on %s\n", ip4addr_ntoa(&cfg.ip));
  return true;
}

void pm_httpd_task(void)
{
  if (!cfg.enabled) return;

  pm_netif_poll();

  for (int i = 0; i < MAX_CONN; i++) {
      if (conns[i].pcb == NULL) continue;
      if (conns[i].st == C_CLOSING) conn_close(&conns[i], false);
      else serve(&conns[i]);
     }
}
