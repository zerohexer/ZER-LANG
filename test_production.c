/* ================================================================
 * Production Firmware Pattern Tests
 *
 * Translated from real production code: MODBUS RTU, SPI flash,
 * CAN frames, I2C sensors, USB state machines, bootloaders,
 * circular DMA buffers, RTOS task patterns.
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "emitter.h"
#include "zercheck.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static bool zer_to_c(const char *zer_source, const char *c_output_path) {
    Arena arena;
    arena_init(&arena, 512 * 1024);
    Scanner scanner;
    scanner_init(&scanner, zer_source);
    Parser parser;
    parser_init(&parser, &scanner, &arena, "test.zer");
    Node *file = parse_file(&parser);
    if (parser.had_error) { arena_free(&arena); return false; }
    Checker checker;
    checker_init(&checker, &arena, "test.zer");
    if (!checker_check(&checker, file)) { arena_free(&arena); return false; }
    ZerCheck zc;
    zercheck_init(&zc, &checker, &arena, "test.zer");
    zercheck_run(&zc, file);
    FILE *out = fopen(c_output_path, "w");
    if (!out) { arena_free(&arena); return false; }
    Emitter emitter;
    emitter_init(&emitter, out, &arena, &checker);
    emit_file(&emitter, file);
    fclose(out);
    arena_free(&arena);
    return true;
}

static void test_e2e(const char *zer_source, int expected_exit, const char *test_name) {
    tests_run++;
    if (!zer_to_c(zer_source, "_zer_prod_test.c")) {
        printf("  FAIL: %s — ZER compilation failed\n", test_name);
        tests_failed++;
        return;
    }
    int gcc_ret = system("gcc -std=c99 -O0 -w -o _zer_prod_test.exe _zer_prod_test.c 2>_zer_prod_err.txt");
    if (gcc_ret != 0) {
        printf("  FAIL: %s — GCC compilation failed\n", test_name);
        FILE *ef = fopen("_zer_prod_err.txt", "r");
        if (ef) {
            char buf[512];
            while (fgets(buf, sizeof(buf), ef)) printf("    gcc: %s", buf);
            fclose(ef);
        }
        tests_failed++;
        return;
    }
    int run_ret = system(".\\_zer_prod_test.exe");
    if (run_ret != expected_exit) {
        printf("  FAIL: %s — expected exit %d, got %d\n",
               test_name, expected_exit, run_ret);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ================================================================
 * PRODUCTION 1: CRC-16/MODBUS (real algorithm, bitwise)
 * Tests: loops, bitwise ops, shifts, pointer indexing, truncation
 * ================================================================ */

static void test_modbus_crc(void) {
    printf("\n--- Production 1: MODBUS CRC-16 ---\n");

    test_e2e(
        "u16 modbus_crc16(*u8 data, u32 length) {\n"
        "    u16 crc = 0xFFFF;\n"
        "    for (u32 i = 0; i < length; i += 1) {\n"
        "        crc = crc ^ @truncate(u16, data[i]);\n"
        "        for (u32 j = 0; j < 8; j += 1) {\n"
        "            if ((crc & 1) != 0) {\n"
        "                crc = (crc >> 1) ^ 0xA001;\n"
        "            } else {\n"
        "                crc = crc >> 1;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return crc;\n"
        "}\n"
        "u32 main() {\n"
        "    u8[4] data;\n"
        "    data[0] = 0x01;\n"
        "    data[1] = 0x03;\n"
        "    data[2] = 0x00;\n"
        "    data[3] = 0x00;\n"
        "    u16 crc = modbus_crc16(&data[0], 4);\n"
        "    if (crc == 0xD8F1) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "MODBUS CRC-16 on [01 03 00 00] = 0xD8F1");
}

/* ================================================================
 * PRODUCTION 2: CRC-8 (Sensirion sensor protocol)
 * ================================================================ */

static void test_crc8(void) {
    printf("\n--- Production 2: CRC-8 (Sensirion) ---\n");

    test_e2e(
        "u8 crc8(*u8 data, u32 len) {\n"
        "    u8 crc = 0xFF;\n"
        "    for (u32 i = 0; i < len; i += 1) {\n"
        "        crc = crc ^ data[i];\n"
        "        for (u32 bit = 0; bit < 8; bit += 1) {\n"
        "            if ((crc & 0x80) != 0) {\n"
        "                crc = (crc << 1) ^ 0x31;\n"
        "            } else {\n"
        "                crc = crc << 1;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return crc;\n"
        "}\n"
        "u32 main() {\n"
        "    u8[2] data;\n"
        "    data[0] = 0xBE;\n"
        "    data[1] = 0xEF;\n"
        "    u8 crc = crc8(&data[0], 2);\n"
        "    if (crc == 0x92) { return 0; }\n"
        "    return @truncate(u32, crc);\n"
        "}\n",
        0, "CRC-8 Sensirion on [BE EF] = 0x92");
}

/* ================================================================
 * PRODUCTION 3: Packed CAN frame (protocol parsing)
 * ================================================================ */

static void test_can_frame(void) {
    printf("\n--- Production 3: CAN frame ---\n");

    test_e2e(
        "packed struct CanFrame {\n"
        "    u32 id;\n"
        "    u8 dlc;\n"
        "    u8 pad;\n"
        "    u8 res0;\n"
        "    u8 res1;\n"
        "}\n"
        "u32 main() {\n"
        "    CanFrame f;\n"
        "    f.id = 0x555;\n"
        "    f.dlc = 5;\n"
        "    f.pad = 0;\n"
        "    f.res0 = 0;\n"
        "    f.res1 = 0;\n"
        "    u32 std_id = f.id & 0x7FF;\n"
        "    if (std_id != 0x555) { return 1; }\n"
        "    if (f.dlc != 5) { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0, "packed CAN frame: id masking + DLC");

    /* CAN filter mask operation */
    test_e2e(
        "struct CanFilter { u32 id; u32 mask; }\n"
        "bool filter_match(u32 msg_id, *CanFilter filter) {\n"
        "    return (msg_id & filter.mask) == (filter.id & filter.mask);\n"
        "}\n"
        "u32 main() {\n"
        "    CanFilter f;\n"
        "    f.id = 0x550;\n"
        "    f.mask = 0xFF0;\n"
        "    if (!filter_match(0x555, &f)) { return 1; }\n"
        "    if (!filter_match(0x55F, &f)) { return 2; }\n"
        "    if (filter_match(0x560, &f)) { return 3; }\n"
        "    return 0;\n"
        "}\n",
        0, "CAN filter mask: 0x550/0xFF0 matches 0x555,0x55F not 0x560");
}

/* ================================================================
 * PRODUCTION 4: USB device state machine (real transitions)
 * ================================================================ */

static void test_usb_state_machine(void) {
    printf("\n--- Production 4: USB state machine ---\n");

    test_e2e(
        "enum UsbState { detached, attached, powered, dflt, address, configured, suspended }\n"
        "enum UsbEvent { attach, reset, set_address, set_config, suspend, resume, detach }\n"
        "struct UsbDevice {\n"
        "    UsbState state;\n"
        "    u8 addr;\n"
        "    u8 config;\n"
        "}\n"
        "UsbState usb_handle(*UsbDevice dev, UsbEvent evt) {\n"
        "    switch (dev.state) {\n"
        "        .detached => {\n"
        "            if (evt == UsbEvent.attach) { dev.state = UsbState.attached; }\n"
        "        }\n"
        "        .attached => {\n"
        "            if (evt == UsbEvent.reset) {\n"
        "                dev.addr = 0;\n"
        "                dev.state = UsbState.dflt;\n"
        "            }\n"
        "        }\n"
        "        .powered => { }\n"
        "        .dflt => {\n"
        "            if (evt == UsbEvent.set_address) {\n"
        "                dev.state = UsbState.address;\n"
        "            }\n"
        "        }\n"
        "        .address => {\n"
        "            if (evt == UsbEvent.set_config) {\n"
        "                dev.state = UsbState.configured;\n"
        "            } else {\n"
        "                if (evt == UsbEvent.reset) {\n"
        "                    dev.addr = 0;\n"
        "                    dev.state = UsbState.dflt;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        .configured => {\n"
        "            if (evt == UsbEvent.reset) {\n"
        "                dev.addr = 0;\n"
        "                dev.config = 0;\n"
        "                dev.state = UsbState.dflt;\n"
        "            }\n"
        "        }\n"
        "        .suspended => {\n"
        "            if (evt == UsbEvent.resume) {\n"
        "                dev.state = UsbState.configured;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return dev.state;\n"
        "}\n"
        "u32 main() {\n"
        "    UsbDevice dev;\n"
        "    dev.state = UsbState.detached;\n"
        "    dev.addr = 0;\n"
        "    dev.config = 0;\n"
        "    usb_handle(&dev, UsbEvent.attach);\n"
        "    if (dev.state != UsbState.attached) { return 1; }\n"
        "    usb_handle(&dev, UsbEvent.reset);\n"
        "    if (dev.state != UsbState.dflt) { return 2; }\n"
        "    usb_handle(&dev, UsbEvent.set_address);\n"
        "    if (dev.state != UsbState.address) { return 3; }\n"
        "    usb_handle(&dev, UsbEvent.set_config);\n"
        "    if (dev.state != UsbState.configured) { return 4; }\n"
        "    usb_handle(&dev, UsbEvent.reset);\n"
        "    if (dev.state != UsbState.dflt) { return 5; }\n"
        "    return 0;\n"
        "}\n",
        0, "USB state machine: detach→attach→reset→addr→config→reset");
}

/* ================================================================
 * PRODUCTION 5: Bootloader state machine with error codes
 * ================================================================ */

static void test_bootloader(void) {
    printf("\n--- Production 5: Bootloader state machine ---\n");

    test_e2e(
        "enum BootState { init, check_app, validate_crc, jump_app, enter_dfu, dfu_idle, dfu_err }\n"
        "enum BootErr { ok, no_app, crc_mismatch, flash_err, timeout }\n"
        "struct BootCtx {\n"
        "    BootState state;\n"
        "    u32 app_crc;\n"
        "    u32 expected_crc;\n"
        "    bool force_dfu;\n"
        "    u8 error_code;\n"
        "}\n"
        "BootErr boot_step(*BootCtx ctx) {\n"
        "    switch (ctx.state) {\n"
        "        .init => {\n"
        "            if (ctx.force_dfu) {\n"
        "                ctx.state = BootState.enter_dfu;\n"
        "            } else {\n"
        "                ctx.state = BootState.check_app;\n"
        "            }\n"
        "        }\n"
        "        .check_app => {\n"
        "            ctx.state = BootState.validate_crc;\n"
        "        }\n"
        "        .validate_crc => {\n"
        "            if (ctx.app_crc == ctx.expected_crc) {\n"
        "                ctx.state = BootState.jump_app;\n"
        "            } else {\n"
        "                ctx.error_code = 1;\n"
        "                ctx.state = BootState.dfu_err;\n"
        "                return BootErr.crc_mismatch;\n"
        "            }\n"
        "        }\n"
        "        .jump_app => { }\n"
        "        .enter_dfu => { ctx.state = BootState.dfu_idle; }\n"
        "        .dfu_idle => { }\n"
        "        .dfu_err => { }\n"
        "    }\n"
        "    return BootErr.ok;\n"
        "}\n"
        "u32 main() {\n"
        "    BootCtx ctx;\n"
        "    ctx.state = BootState.init;\n"
        "    ctx.app_crc = 0xDEAD;\n"
        "    ctx.expected_crc = 0xDEAD;\n"
        "    ctx.force_dfu = false;\n"
        "    ctx.error_code = 0;\n"
        "    BootErr e = boot_step(&ctx);\n"
        "    if (e != BootErr.ok) { return 1; }\n"
        "    if (ctx.state != BootState.check_app) { return 2; }\n"
        "    e = boot_step(&ctx);\n"
        "    if (ctx.state != BootState.validate_crc) { return 3; }\n"
        "    e = boot_step(&ctx);\n"
        "    if (ctx.state != BootState.jump_app) { return 4; }\n"
        "    return 0;\n"
        "}\n",
        0, "bootloader: init→check→validate(match)→jump");

    /* CRC mismatch path */
    test_e2e(
        "enum BootState { init, check_app, validate_crc, jump_app, enter_dfu, dfu_idle, dfu_err }\n"
        "enum BootErr { ok, no_app, crc_mismatch, flash_err, timeout }\n"
        "struct BootCtx {\n"
        "    BootState state;\n"
        "    u32 app_crc;\n"
        "    u32 expected_crc;\n"
        "    bool force_dfu;\n"
        "    u8 error_code;\n"
        "}\n"
        "BootErr boot_step(*BootCtx ctx) {\n"
        "    switch (ctx.state) {\n"
        "        .init => { ctx.state = BootState.check_app; }\n"
        "        .check_app => { ctx.state = BootState.validate_crc; }\n"
        "        .validate_crc => {\n"
        "            if (ctx.app_crc == ctx.expected_crc) {\n"
        "                ctx.state = BootState.jump_app;\n"
        "            } else {\n"
        "                ctx.state = BootState.dfu_err;\n"
        "                return BootErr.crc_mismatch;\n"
        "            }\n"
        "        }\n"
        "        .jump_app => { }\n"
        "        .enter_dfu => { }\n"
        "        .dfu_idle => { }\n"
        "        .dfu_err => { }\n"
        "    }\n"
        "    return BootErr.ok;\n"
        "}\n"
        "u32 main() {\n"
        "    BootCtx ctx;\n"
        "    ctx.state = BootState.init;\n"
        "    ctx.app_crc = 0xDEAD;\n"
        "    ctx.expected_crc = 0xBEEF;\n"
        "    ctx.force_dfu = false;\n"
        "    ctx.error_code = 0;\n"
        "    boot_step(&ctx);\n"
        "    boot_step(&ctx);\n"
        "    BootErr e = boot_step(&ctx);\n"
        "    if (e != BootErr.crc_mismatch) { return 1; }\n"
        "    if (ctx.state != BootState.dfu_err) { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0, "bootloader: CRC mismatch → dfu_err");
}

/* ================================================================
 * PRODUCTION 6: SPI flash driver pattern (CS toggle + commands)
 * ================================================================ */

static void test_spi_flash_pattern(void) {
    printf("\n--- Production 6: SPI flash driver ---\n");

    test_e2e(
        "enum FlashErr { ok, spi_err, timeout, invalid }\n"
        "struct FlashDev {\n"
        "    u8 mfg_id;\n"
        "    u16 dev_id;\n"
        "    u32 sector_size;\n"
        "    u32 sector_count;\n"
        "    bool busy;\n"
        "}\n"
        "u32 cs_toggles = 0;\n"
        "void cs_low() { cs_toggles += 1; }\n"
        "void cs_high() { cs_toggles += 1; }\n"
        "FlashErr flash_write_enable(*FlashDev dev) {\n"
        "    cs_low();\n"
        "    defer cs_high();\n"
        "    return FlashErr.ok;\n"
        "}\n"
        "FlashErr flash_wait_busy(*FlashDev dev, u32 max_polls) {\n"
        "    for (u32 i = 0; i < max_polls; i += 1) {\n"
        "        cs_low();\n"
        "        defer cs_high();\n"
        "        if (!dev.busy) { return FlashErr.ok; }\n"
        "    }\n"
        "    return FlashErr.timeout;\n"
        "}\n"
        "FlashErr flash_erase_sector(*FlashDev dev, u32 addr) {\n"
        "    FlashErr e = flash_write_enable(dev);\n"
        "    if (e != FlashErr.ok) { return e; }\n"
        "    cs_low();\n"
        "    defer cs_high();\n"
        "    dev.busy = true;\n"
        "    return FlashErr.ok;\n"
        "}\n"
        "u32 main() {\n"
        "    FlashDev dev;\n"
        "    dev.mfg_id = 0xEF;\n"
        "    dev.dev_id = 0x4018;\n"
        "    dev.sector_size = 4096;\n"
        "    dev.sector_count = 4096;\n"
        "    dev.busy = false;\n"
        "    FlashErr e = flash_erase_sector(&dev, 0x1000);\n"
        "    if (e != FlashErr.ok) { return 1; }\n"
        "    if (cs_toggles != 4) { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0, "SPI flash: CS toggle counting with defer");
}

/* ================================================================
 * PRODUCTION 7: Circular buffer with full/empty detection
 * ================================================================ */

static void test_circular_buffer(void) {
    printf("\n--- Production 7: Circular buffer ---\n");

    test_e2e(
        "struct CircBuf {\n"
        "    u32 head;\n"
        "    u32 tail;\n"
        "    u32 max;\n"
        "    bool full;\n"
        "}\n"
        "u8[16] cbuf_data;\n"
        "CircBuf cbuf;\n"
        "void cbuf_init() {\n"
        "    cbuf.head = 0;\n"
        "    cbuf.tail = 0;\n"
        "    cbuf.max = 16;\n"
        "    cbuf.full = false;\n"
        "}\n"
        "bool cbuf_empty() {\n"
        "    return !cbuf.full && cbuf.head == cbuf.tail;\n"
        "}\n"
        "void cbuf_put(u8 val) {\n"
        "    cbuf_data[cbuf.head] = val;\n"
        "    if (cbuf.full) {\n"
        "        cbuf.tail = (cbuf.tail + 1) % cbuf.max;\n"
        "    }\n"
        "    cbuf.head = (cbuf.head + 1) % cbuf.max;\n"
        "    cbuf.full = cbuf.head == cbuf.tail;\n"
        "}\n"
        "?u8 cbuf_get() {\n"
        "    if (cbuf_empty()) { return null; }\n"
        "    u8 val = cbuf_data[cbuf.tail];\n"
        "    cbuf.full = false;\n"
        "    cbuf.tail = (cbuf.tail + 1) % cbuf.max;\n"
        "    return val;\n"
        "}\n"
        "u32 main() {\n"
        "    cbuf_init();\n"
        "    cbuf_put(0xAA);\n"
        "    cbuf_put(0xBB);\n"
        "    cbuf_put(0xCC);\n"
        "    u8 a = cbuf_get() orelse 0;\n"
        "    u8 b = cbuf_get() orelse 0;\n"
        "    u8 c = cbuf_get() orelse 0;\n"
        "    if (a != 0xAA) { return 1; }\n"
        "    if (b != 0xBB) { return 2; }\n"
        "    if (c != 0xCC) { return 3; }\n"
        "    if (!cbuf_empty()) { return 4; }\n"
        "    return 0;\n"
        "}\n",
        0, "circular buffer: put 3, get 3, check empty");
}

/* ================================================================
 * PRODUCTION 8: I2C sensor retry + calibration parse
 * ================================================================ */

static void test_sensor_driver(void) {
    printf("\n--- Production 8: Sensor driver ---\n");

    test_e2e(
        "enum SensorErr { ok, i2c_err, crc_err, timeout, range_err }\n"
        "struct CalData {\n"
        "    u16 dig_T1;\n"
        "    i16 dig_T2;\n"
        "    i16 dig_T3;\n"
        "}\n"
        "struct SensorDev {\n"
        "    u8 addr;\n"
        "    CalData cal;\n"
        "    bool initialized;\n"
        "}\n"
        "u32 retry_count = 0;\n"
        "SensorErr i2c_read(*SensorDev dev, u8 reg, *u8 buf, u32 len, u8 retries) {\n"
        "    for (u8 attempt = 0; attempt < retries; attempt += 1) {\n"
        "        retry_count += 1;\n"
        "        if (attempt >= 1) { return SensorErr.ok; }\n"
        "    }\n"
        "    return SensorErr.i2c_err;\n"
        "}\n"
        "SensorErr sensor_init(*SensorDev dev, u8 addr) {\n"
        "    dev.addr = addr;\n"
        "    dev.initialized = false;\n"
        "    u8[2] id_buf;\n"
        "    SensorErr e = i2c_read(dev, 0xD0, &id_buf[0], 1, 3);\n"
        "    if (e != SensorErr.ok) { return e; }\n"
        "    dev.initialized = true;\n"
        "    return SensorErr.ok;\n"
        "}\n"
        "u32 main() {\n"
        "    SensorDev dev;\n"
        "    SensorErr e = sensor_init(&dev, 0x76);\n"
        "    if (e != SensorErr.ok) { return 1; }\n"
        "    if (!dev.initialized) { return 2; }\n"
        "    if (dev.addr != 0x76) { return 3; }\n"
        "    if (retry_count != 2) { return 4; }\n"
        "    return 0;\n"
        "}\n",
        0, "sensor init with I2C retry (succeeds on attempt 2)");
}

/* ================================================================
 * PRODUCTION 9: MODBUS frame builder + validator
 * ================================================================ */

static void test_modbus_frame(void) {
    printf("\n--- Production 9: MODBUS frame ---\n");

    test_e2e(
        "u16 modbus_crc(*u8 data, u32 len) {\n"
        "    u16 crc = 0xFFFF;\n"
        "    for (u32 i = 0; i < len; i += 1) {\n"
        "        crc = crc ^ @truncate(u16, data[i]);\n"
        "        for (u32 j = 0; j < 8; j += 1) {\n"
        "            if ((crc & 1) != 0) {\n"
        "                crc = (crc >> 1) ^ 0xA001;\n"
        "            } else {\n"
        "                crc = crc >> 1;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return crc;\n"
        "}\n"
        "void build_frame(*u8 frame, u8 addr, u8 func, u16 start, u16 qty) {\n"
        "    frame[0] = addr;\n"
        "    frame[1] = func;\n"
        "    frame[2] = @truncate(u8, start >> 8);\n"
        "    frame[3] = @truncate(u8, start & 0xFF);\n"
        "    frame[4] = @truncate(u8, qty >> 8);\n"
        "    frame[5] = @truncate(u8, qty & 0xFF);\n"
        "    u16 crc = modbus_crc(frame, 6);\n"
        "    frame[6] = @truncate(u8, crc & 0xFF);\n"
        "    frame[7] = @truncate(u8, crc >> 8);\n"
        "}\n"
        "bool validate_frame(*u8 frame, u32 len) {\n"
        "    if (len < 4) { return false; }\n"
        "    u16 received = @truncate(u16, frame[len - 2]) | (@truncate(u16, frame[len - 1]) << 8);\n"
        "    u16 calculated = modbus_crc(frame, len - 2);\n"
        "    return received == calculated;\n"
        "}\n"
        "u32 main() {\n"
        "    u8[8] frame;\n"
        "    build_frame(&frame[0], 1, 3, 0x0000, 10);\n"
        "    if (!validate_frame(&frame[0], 8)) { return 1; }\n"
        "    if (frame[0] != 1) { return 2; }\n"
        "    if (frame[1] != 3) { return 3; }\n"
        "    frame[3] = 0xFF;\n"
        "    if (validate_frame(&frame[0], 8)) { return 4; }\n"
        "    return 0;\n"
        "}\n",
        0, "MODBUS frame build + CRC validate + tamper detect");
}

/* ================================================================
 * PRODUCTION 10: RTOS-style task pool with run queue
 * ================================================================ */

static void test_rtos_scheduler(void) {
    printf("\n--- Production 10: RTOS scheduler ---\n");

    test_e2e(
        "enum TaskState { ready, running, blocked, terminated }\n"
        "struct Task {\n"
        "    u32 id;\n"
        "    u32 priority;\n"
        "    TaskState state;\n"
        "    u32 work_done;\n"
        "}\n"
        "Pool(Task, 8) task_pool;\n"
        "u32 tick_count = 0;\n"
        "void task_tick(*Task t) {\n"
        "    if (t.state == TaskState.running) {\n"
        "        t.work_done += 1;\n"
        "        tick_count += 1;\n"
        "    }\n"
        "}\n"
        "u32 main() {\n"
        "    Handle(Task) h1 = task_pool.alloc() orelse return;\n"
        "    Handle(Task) h2 = task_pool.alloc() orelse return;\n"
        "    task_pool.get(h1).id = 1;\n"
        "    task_pool.get(h1).priority = 10;\n"
        "    task_pool.get(h1).state = TaskState.running;\n"
        "    task_pool.get(h1).work_done = 0;\n"
        "    task_pool.get(h2).id = 2;\n"
        "    task_pool.get(h2).priority = 5;\n"
        "    task_pool.get(h2).state = TaskState.running;\n"
        "    task_pool.get(h2).work_done = 0;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        task_tick(task_pool.get(h1));\n"
        "        task_tick(task_pool.get(h2));\n"
        "    }\n"
        "    u32 total = task_pool.get(h1).work_done + task_pool.get(h2).work_done;\n"
        "    task_pool.free(h1);\n"
        "    task_pool.free(h2);\n"
        "    if (tick_count != 10) { return 1; }\n"
        "    return total - 10;\n"
        "}\n",
        0, "RTOS scheduler: 2 tasks, 5 ticks each = 10 total");
}

/* ================================================================
 * PRODUCTION 11: DMA-style double buffer swap
 * ================================================================ */

static void test_double_buffer(void) {
    printf("\n--- Production 11: Double buffer ---\n");

    test_e2e(
        "u8[64] buf_a;\n"
        "u8[64] buf_b;\n"
        "u32 active = 0;\n"
        "u32 processed = 0;\n"
        "void fill_buffer(*u8 buf, u8 val, u32 len) {\n"
        "    for (u32 i = 0; i < len; i += 1) {\n"
        "        buf[i] = val;\n"
        "    }\n"
        "}\n"
        "u32 process_buffer(*u8 buf, u32 len) {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < len; i += 1) {\n"
        "        sum += @truncate(u32, buf[i]);\n"
        "    }\n"
        "    processed += 1;\n"
        "    return sum;\n"
        "}\n"
        "u32 main() {\n"
        "    fill_buffer(&buf_a[0], 1, 64);\n"
        "    fill_buffer(&buf_b[0], 2, 64);\n"
        "    u32 sum_a = process_buffer(&buf_a[0], 64);\n"
        "    u32 sum_b = process_buffer(&buf_b[0], 64);\n"
        "    if (sum_a != 64) { return 1; }\n"
        "    if (sum_b != 128) { return 2; }\n"
        "    if (processed != 2) { return 3; }\n"
        "    return 0;\n"
        "}\n",
        0, "double buffer: fill + process + swap");
}

/* ================================================================
 * PRODUCTION 12: Byte-level protocol parser (little-endian)
 * ================================================================ */

static void test_protocol_parser(void) {
    printf("\n--- Production 12: Protocol parser ---\n");

    test_e2e(
        "struct PacketHeader {\n"
        "    u8 sync;\n"
        "    u8 msg_type;\n"
        "    u16 length;\n"
        "}\n"
        "PacketHeader parse_header(*u8 raw) {\n"
        "    PacketHeader h;\n"
        "    h.sync = raw[0];\n"
        "    h.msg_type = raw[1];\n"
        "    h.length = @truncate(u16, raw[2]) | (@truncate(u16, raw[3]) << 8);\n"
        "    return h;\n"
        "}\n"
        "u32 main() {\n"
        "    u8[8] raw;\n"
        "    raw[0] = 0xAA;\n"
        "    raw[1] = 0x03;\n"
        "    raw[2] = 0x10;\n"
        "    raw[3] = 0x00;\n"
        "    PacketHeader h = parse_header(&raw[0]);\n"
        "    if (h.sync != 0xAA) { return 1; }\n"
        "    if (h.msg_type != 3) { return 2; }\n"
        "    if (h.length != 16) { return 3; }\n"
        "    return 0;\n"
        "}\n",
        0, "protocol header: parse LE u16 from bytes");
}

/* ================================================================ */

int main(void) {
    printf("=== Production Firmware Pattern Tests ===\n");

    test_modbus_crc();
    test_crc8();
    test_can_frame();
    test_usb_state_machine();
    test_bootloader();
    test_spi_flash_pattern();
    test_circular_buffer();
    test_sensor_driver();
    test_modbus_frame();
    test_rtos_scheduler();
    test_double_buffer();
    test_protocol_parser();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    remove("_zer_prod_test.c");
    remove("_zer_prod_test.exe");
    remove("_zer_prod_test.o");
    remove("_zer_prod_err.txt");

    return tests_failed > 0 ? 1 : 0;
}
