/*
 * Bramble emu-link bridge (QEMU esp32s3).
 *
 * The QEMU pager's single emu-link connection to the gosim ether, shared by the
 * device models (the SX1262 radio and the indicator bridge). The QEMU node is an
 * emu-link client exactly like the linux node (components/emu_link/emu_link.c):
 * JSON, one object per line, over a socket. Here the socket is a QEMU chardev
 * ("emulink") the gosim supervisor wires with
 *   -chardev socket,id=emulink,path=<broker.sock>,server=off
 * so QEMU dials the broker's unix listener the way the linux node dials
 * EMU_BROKER. The broker is one-hello-per-node, so there is exactly one link.
 *
 * On socket open (CHR_EVENT_OPENED) it sends hello{node,version:1,fw,caps};
 * inbound lines are split on '\n', parsed with QEMU's qjson, and dispatched by
 * their "t" field to handlers device models register (emulink_on); the SX1262
 * model calls emulink_send_tx() to key the channel. node id comes from the
 * BRAMBLE_EMU_NODE env var the supervisor sets per instance (default
 * "qemu-pager"); the broker binds a node to a reserved SLOT by position, not by
 * this id, so it only affects UI / console tagging.
 *
 * This is the transport TU: framing, the handler table, and the `tx`/`fb`/raw
 * send helpers. Message builders that own device state (the indicator `ind`)
 * live with that state and call emulink_write() here.
 *
 * Threading: the chardev receive/event callbacks run on the QEMU main loop with
 * the BQL held, and the SX1262 SPI transfers that call emulink_send_tx run on
 * the vCPU thread under the BQL, so the BQL serializes the two and the shared
 * state needs no extra lock. qemu_chr_fe_write_all is documented thread-safe.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qobject.h"
#include "hw/xtensa/bramble_emulink.h"

#define EMULINK_PROTOCOL_VERSION 1
#define EMULINK_CHARDEV_ID       "emulink"
#define EMULINK_MAX_HANDLERS     8
#define EMULINK_MAX_TYPE_LEN     15
#define EMULINK_MAX_LINE         65536

typedef struct {
    bool used;
    char type[EMULINK_MAX_TYPE_LEN + 1];
    emulink_handler_t fn;
    void *ctx;
} EmulinkHandler;

static CharBackend s_emulink_chr;
static bool s_emulink_have_chr;
static bool s_emulink_open;
static GString *s_emulink_rxbuf;
static EmulinkHandler s_emulink_handlers[EMULINK_MAX_HANDLERS];

int emulink_write(const char *s, size_t len)
{
    if (!s_emulink_have_chr || !s_emulink_open) {
        return -1;
    }
    return qemu_chr_fe_write_all(&s_emulink_chr, (const uint8_t *)s, (int)len);
}

/* Emit a `tx`: PHY bytes base64-encoded, plus the latched modulation params
 * (freq MHz, sf, bw Hz, cr, power dBm). No-op if the link is not connected, so
 * a standalone boot's radio simply never gets txdone and times out. */
int emulink_send_tx(const uint8_t *payload, unsigned len, int freq_mhz,
                    int sf, int bw_hz, int cr, int power)
{
    if (!payload || len == 0) {
        return -1;
    }
    g_autofree char *b64 = g_base64_encode(payload, len);
    g_autofree char *line = g_strdup_printf(
        "{\"t\":\"tx\",\"payload\":\"%s\",\"freq\":%d,\"sf\":%d,\"bw\":%d,"
        "\"cr\":%d,\"power\":%d}\n",
        b64, freq_mhz, sf, bw_hz, cr, power);
    return emulink_write(line, strlen(line));
}

/* Emit an `fb`: the resolved 1bpp logical framebuffer base64-encoded, plus the
 * refresh kind ("full"/"partial") and busy duration, matching display_virt.c's
 * message shape exactly so the browser renders the QEMU pager's e-paper
 * identically to a linux node. No-op if the link is not connected. */
int emulink_send_fb(const uint8_t *fb, size_t fb_len, uint32_t seq,
                    const char *kind, uint32_t busy_ms)
{
    if (!fb || fb_len == 0) {
        return -1;
    }
    g_autofree char *b64 = g_base64_encode(fb, fb_len);
    g_autofree char *line = g_strdup_printf(
        "{\"t\":\"fb\",\"seq\":%u,\"kind\":\"%s\",\"fb\":\"%s\","
        "\"busy_ms\":%u}\n",
        seq, kind, b64, busy_ms);
    return emulink_write(line, strlen(line));
}

int emulink_on(const char *type, emulink_handler_t fn, void *ctx)
{
    if (!type || !fn || strlen(type) > EMULINK_MAX_TYPE_LEN) {
        return -1;
    }
    int free_slot = -1;
    for (int i = 0; i < EMULINK_MAX_HANDLERS; i++) {
        if (s_emulink_handlers[i].used &&
            strcmp(s_emulink_handlers[i].type, type) == 0) {
            s_emulink_handlers[i].fn = fn;
            s_emulink_handlers[i].ctx = ctx;
            return 0;
        }
        if (!s_emulink_handlers[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return -1;
    }
    s_emulink_handlers[free_slot].used = true;
    pstrcpy(s_emulink_handlers[free_slot].type,
            sizeof(s_emulink_handlers[free_slot].type), type);
    s_emulink_handlers[free_slot].fn = fn;
    s_emulink_handlers[free_slot].ctx = ctx;
    return 0;
}

static void emulink_send_hello(void)
{
    const char *node = getenv("BRAMBLE_EMU_NODE");
    if (!node || !*node) {
        node = "qemu-pager";
    }
    g_autofree char *hello = g_strdup_printf(
        "{\"t\":\"hello\",\"node\":\"%s\",\"version\":%d,\"fw\":\"qemu\","
        "\"caps\":\"radio,display,buttons,gps,battery\"}\n",
        node, EMULINK_PROTOCOL_VERSION);
    (void)emulink_write(hello, strlen(hello));
    fprintf(stderr, "bramble-emulink: hello sent as node=%s\n", node);
}

static void emulink_dispatch_line(const char *line)
{
    if (!*line) {
        return;
    }
    QObject *obj = qobject_from_json(line, NULL);
    if (!obj) {
        return; /* malformed: ignore (forward compat) */
    }
    QDict *msg = qobject_to(QDict, obj);
    if (msg) {
        const char *t = qdict_get_try_str(msg, "t");
        if (t) {
            for (int i = 0; i < EMULINK_MAX_HANDLERS; i++) {
                if (s_emulink_handlers[i].used &&
                    strcmp(s_emulink_handlers[i].type, t) == 0) {
                    s_emulink_handlers[i].fn(msg, s_emulink_handlers[i].ctx);
                    break;
                }
            }
        }
    }
    qobject_unref(obj);
}

static int emulink_can_receive(void *opaque)
{
    (void)opaque;
    return EMULINK_MAX_LINE;
}

static void emulink_receive(void *opaque, const uint8_t *buf, int size)
{
    (void)opaque;
    for (int i = 0; i < size; i++) {
        char c = (char)buf[i];
        if (c == '\n') {
            emulink_dispatch_line(s_emulink_rxbuf->str);
            g_string_truncate(s_emulink_rxbuf, 0);
        } else {
            if (s_emulink_rxbuf->len >= EMULINK_MAX_LINE) {
                g_string_truncate(s_emulink_rxbuf, 0); /* oversized: resync */
            }
            g_string_append_c(s_emulink_rxbuf, c);
        }
    }
}

static void emulink_event(void *opaque, QEMUChrEvent event)
{
    (void)opaque;
    switch (event) {
    case CHR_EVENT_OPENED:
        s_emulink_open = true;
        g_string_truncate(s_emulink_rxbuf, 0);
        emulink_send_hello();
        break;
    case CHR_EVENT_CLOSED:
        s_emulink_open = false;
        break;
    default:
        break;
    }
}

void bramble_emulink_attach(void)
{
    Chardev *chr = qemu_chr_find(EMULINK_CHARDEV_ID);
    if (!chr) {
        fprintf(stderr, "bramble-emulink: no '%s' chardev; ether bridge idle\n",
                EMULINK_CHARDEV_ID);
        return;
    }
    if (!qemu_chr_fe_init(&s_emulink_chr, chr, &error_abort)) {
        fprintf(stderr, "bramble-emulink: chardev init failed\n");
        return;
    }
    s_emulink_have_chr = true;
    s_emulink_rxbuf = g_string_new(NULL);
    /* set_open=true replays a CHR_EVENT_OPENED if the socket connected during
     * option parse (server=off dials immediately), so hello is not missed. */
    qemu_chr_fe_set_handlers(&s_emulink_chr, emulink_can_receive,
                             emulink_receive, emulink_event, NULL, NULL, NULL,
                             true);
    fprintf(stderr, "bramble-emulink: bridge attached to '%s' chardev\n",
            EMULINK_CHARDEV_ID);
}
