#include "../../core/kernel.h"
#include "rtc.h"

static int rtc_initialized = 0;

static int rtc_is_update_in_progress(void) {
    outb(CMOS_ADDR, RTC_STATUS_A);
    return inb(CMOS_DATA) & RTC_UIP;
}

uint8_t rtc_read_register(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

void rtc_init(void) {
    outb(CMOS_ADDR, RTC_STATUS_B);
    io_wait();
    uint8_t reg_b = inb(CMOS_DATA);

    // Set 24-hour mode (bit 1) and binary mode (bit 2)
    reg_b |= 0x06;
    outb(CMOS_ADDR, RTC_STATUS_B);
    io_wait();
    outb(CMOS_DATA, reg_b);

    rtc_initialized = 1;
}

// Pure: decode the six raw CMOS register bytes into a rtc_time_t, honouring Status-B's
// binary/BCD flag (bit 2) and 24h/12h flag (bit 1). In 12-hour mode the raw hour's TOP BIT
// is the PM flag, and it must be stripped BEFORE any BCD decode — BCD-decoding a byte with
// bit 7 set folds that bit into the tens digit and corrupts the value (the old code decoded
// BCD first, then tested a PM bit that was already gone, so a 12-hour-BCD RTC read the wrong
// hour: latent because rtc_init forces 24h+binary, but reachable on real-HW firmware that
// ignores that write). Extracted so the `rtc` KAT can pin every mode. `mode24`/`binary` are
// the decoded Status-B bits. Split off both the fix and its test surface.
void rtc_decode(rtc_time_t* t, uint8_t s, uint8_t mi, uint8_t h,
                uint8_t d, uint8_t mo, uint8_t y, int binary, int mode24) {
    int pm = 0;
    if (!mode24) { pm = (h & 0x80) != 0; h &= 0x7F; }        // strip PM first (before BCD)
    if (!binary) {
        s  = (uint8_t)((s  >> 4) * 10 + (s  & 0x0F));
        mi = (uint8_t)((mi >> 4) * 10 + (mi & 0x0F));
        h  = (uint8_t)((h  >> 4) * 10 + (h  & 0x0F));
        d  = (uint8_t)((d  >> 4) * 10 + (d  & 0x0F));
        mo = (uint8_t)((mo >> 4) * 10 + (mo & 0x0F));
        y  = (uint8_t)((y  >> 4) * 10 + (y  & 0x0F));
    }
    if (!mode24) {                                           // 12-hour -> 24-hour
        if (h == 12) h = 0;                                 // 12 AM = 00:00 (and 12 PM -> 0 -> +12)
        if (pm) h += 12;                                    // 1..11 PM -> 13..23
    }
    t->second = s; t->minute = mi; t->hour = h;
    t->day = d; t->month = mo; t->year = 2000 + y;          // assume 2000-2099
}

void rtc_read_time(rtc_time_t* t) {
    if (!t) return;

    uint8_t reg_b = rtc_read_register(RTC_STATUS_B);
    uint8_t second, minute, hour, day, month, year;

    // Read all registers atomically (retry if UIP was set during read)
    int tries = 3;
    do {
        // Bounded wait for the update-in-progress flag to clear (normally < 2 ms). Unbounded
        // here could hang the boot on real hardware whose RTC never clears UIP as expected.
        for (int w = 0; w < 1000000 && rtc_is_update_in_progress(); w++);

        second = rtc_read_register(RTC_SECONDS);
        minute = rtc_read_register(RTC_MINUTES);
        hour   = rtc_read_register(RTC_HOURS);
        day    = rtc_read_register(RTC_DAY);
        month  = rtc_read_register(RTC_MONTH);
        year   = rtc_read_register(RTC_YEAR);

        // If UIP was still clear during entire read, we're good
        if (!rtc_is_update_in_progress()) break;
    } while (--tries);

    rtc_decode(t, second, minute, hour, day, month, year,
               (reg_b & 0x04) != 0, (reg_b & 0x02) != 0);
}

// KAT for rtc_decode — pins BCD/binary and 24h/12h decoding, especially the 12-hour PM path
// that the old ordering corrupted. Returns 0 on pass, else the failing case number.
int rtc_selftest(void) {
    rtc_time_t t;
    // 24-hour binary: passthrough
    rtc_decode(&t, 30, 45, 13, 27, 8, 26, 1, 1);
    if (t.second != 30 || t.minute != 45 || t.hour != 13 || t.day != 27 || t.month != 8 || t.year != 2026) return 1;
    // 24-hour BCD: 0x59 sec -> 59, 0x23 hour -> 23, 0x26 year -> 2026
    rtc_decode(&t, 0x59, 0x00, 0x23, 0x31, 0x12, 0x26, 0, 1);
    if (t.second != 59 || t.hour != 23 || t.day != 31 || t.month != 12 || t.year != 2026) return 2;
    // 12-hour BCD, 1 PM: hour byte 0x81 (PM | BCD 01) -> 13 (this is what the old order broke)
    rtc_decode(&t, 0, 0, 0x81, 1, 1, 0x24, 0, 0);
    if (t.hour != 13) return 3;
    // 12-hour BCD, 12 AM (midnight): 0x12 (no PM, BCD 12) -> 0
    rtc_decode(&t, 0, 0, 0x12, 1, 1, 0, 0, 0);
    if (t.hour != 0) return 4;
    // 12-hour BCD, 12 PM (noon): 0x92 (PM | BCD 12) -> 12
    rtc_decode(&t, 0, 0, 0x92, 1, 1, 0, 0, 0);
    if (t.hour != 12) return 5;
    // 12-hour BCD, 11 PM: 0x91 (PM | BCD 11) -> 23
    rtc_decode(&t, 0, 0, 0x91, 1, 1, 0, 0, 0);
    if (t.hour != 23) return 6;
    // 12-hour binary, 3 PM: 0x83 (PM | 3) -> 15
    rtc_decode(&t, 0, 0, 0x83, 1, 1, 1, 0, 0);
    if (t.hour != 15) return 7;
    return 0;
}
