/*
 * The fourth encode, and the reason it is opened rather than linked.
 *
 * Two builds of x264 end up in this process: drc-x264 (libx264.so.140),
 * which is the only one a Wii U GamePad can decode, and the
 * distribution's (164), which arrives with libavcodec. x264 versions
 * exactly ONE exported symbol -- x264_encoder_open -- and its own header
 * says the suffix exists "for purposes of dlopen". x264_encoder_encode
 * and the rest are bare names.
 *
 * So the danger is not theoretical and it is not visible: an encoder
 * opened by one build and driven by the other's code corrupts silently.
 * This pins down that drc_encoder.c keeps them apart, and that it still
 * produces what the pad's protocol demands.
 *
 * Skipped, not failed, where drc-x264 is absent: a machine with no
 * Realtek adapter has no use for this chain, and the host is meant to
 * lose it and nothing else.
 */
#include "test_util.h"

#include <dlfcn.h>

#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../drc_encoder.c"

/* A picture with something in it. A flat field encodes to almost
 * nothing and would pass this test while telling us very little. */
static void fill_pattern(uint8_t *i420, int t) {
    const size_t y_size = (size_t)DRC_ENC_WIDTH * DRC_ENC_HEIGHT;
    for (int y = 0; y < DRC_ENC_HEIGHT; y++) {
        for (int x = 0; x < DRC_ENC_WIDTH; x++) {
            i420[(size_t)y * DRC_ENC_WIDTH + x] =
                (uint8_t)((x * 3 + y * 5 + t * 17) ^ (x >> 2));
        }
    }
    memset(i420 + y_size, 128, y_size / 2);
}

int main(void) {
    t_begin("the wii u encode, and the two x264 builds it lives beside");

    /*
     * Touching libavcodec on purpose, and not only to print a number:
     * the linker drops a library nothing calls (--as-needed), and a
     * test that quietly failed to load the second x264 would assert
     * isolation from a library that was never there.
     */
    printf("  libavcodec %u.%u, which brings the system x264\n",
           avcodec_version() >> 16, (avcodec_version() >> 8) & 0xff);

    char err[256];
    DrcEncoder *e = drc_encoder_open(NULL, NULL, err, sizeof(err));
    if (!e) {
        printf("  skipped: %s\n", err);
        return 0;
    }
    printf("  drc-x264: %s\n", drc_encoder_library(e));

    /*
     * The isolation itself.
     *
     * RTLD_LOCAL means this library is not in the global lookup scope,
     * so a lookup that does not go through the handle must NOT find it.
     * If this ever finds something, the encoder is reachable by name
     * and the two builds can answer for each other.
     */
    void *global_open_140 = dlsym(RTLD_DEFAULT, "x264_encoder_open_140");
    t_ok("drc-x264 leaked into the global symbol scope", global_open_140 == NULL);

    /*
     * And the other build really is here, or this test proves nothing.
     *
     * This file is linked against libavcodec exactly as the host is, so
     * the distribution's libx264.so.164 is loaded too. Its versioned
     * symbol is globally visible; the fork's is not. That asymmetry IS
     * the isolation -- both builds resident, neither able to answer for
     * the other.
     */
    t_ok("the system x264 is not loaded, so nothing is being kept apart",
         dlsym(RTLD_DEFAULT, "x264_encoder_open_164") != NULL);

    /* And the handle really did give us the fork, not the system's
     * library under a name that happens to resolve. */
    t_ok("the opened library is not drc-x264", dlsym(e->lib, "x264_encoder_open_140") != NULL);

    const size_t frame_size = (size_t)DRC_ENC_WIDTH * DRC_ENC_HEIGHT * 3 / 2;
    uint8_t *i420 = malloc(frame_size);
    t_ok("out of memory", i420 != NULL);

    /* Five chunks, every frame, or the pad has nothing it can use. */
    int idr_seen = 0;
    for (int t = 0; t < 12; t++) {
        fill_pattern(i420, t);
        DrcFrame f;
        t_ok("the encoder did not produce a frame", drc_encoder_encode(e, i420, t == 0, &f) == 0);
        for (int i = 0; i < DRC_ENC_CHUNKS; i++) {
            t_ok("a chunk came back empty", f.chunk[i] != NULL && f.size[i] > 0);
        }
        if (f.is_idr) {
            idr_seen++;
        }
    }
    t_ok("no frame ever came out as a recovery point", idr_seen >= 1);

    /*
     * Restart() is the only way back.
     *
     * Under intra refresh x264 answers a forced IDR by restarting its
     * refresh wave and never emits NAL_SLICE_IDR again -- which leaves
     * a decoder that lost the sequence frozen for good while the sound
     * plays on. A fresh encoder must genuinely produce one.
     */
    t_ok("the encoder would not restart", drc_encoder_restart(e) == 0);
    fill_pattern(i420, 99);
    DrcFrame first;
    t_ok("no frame after a restart", drc_encoder_encode(e, i420, 1, &first) == 0);
    t_ok("a restarted encoder did not produce a recovery point", first.is_idr);

    /*
     * And the real question: is any of this decodable?
     *
     * Five non-empty chunks prove the encoder ran, not that it produced
     * H.264 anybody can read -- and the pad says nothing when it
     * cannot: no error, no request, just a dark panel. So the frames
     * are written out in the container libdrc's own debug dump uses and
     * handed to ffmpeg, which is the only thing here that will complain
     * out loud.
     *
     * DRH mode emits no slice header and no parameter sets, so both are
     * reconstructed exactly as libdrc's DumpH264Frame does: the pad's
     * hardcoded SPS and PPS first, then a slice header per frame, with
     * the payload escaped.
     */
    char dump_path[256];
    snprintf(dump_path, sizeof(dump_path), "/tmp/c2c_drc_test_%d.h264", (int)getpid());
    FILE *dump = fopen(dump_path, "wb");
    t_ok("could not open the dump", dump != NULL);
    if (dump) {
        static const uint8_t start[] = { 0, 0, 0, 1 };
        static const uint8_t sps[] = { 0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40,
                                       0x6c, 0x1e, 0xf3, 0x68 };
        static const uint8_t pps[] = { 0x68, 0xee, 0x06, 0x0c, 0xe8 };
        fwrite(start, sizeof(start), 1, dump);
        fwrite(sps, sizeof(sps), 1, dump);
        fwrite(start, sizeof(start), 1, dump);
        fwrite(pps, sizeof(pps), 1, dump);

        t_ok("a fresh encoder for the dump", drc_encoder_restart(e) == 0);
        uint32_t frame_number = 0;
        int written = 0;
        for (int t = 0; t < 30; t++) {
            fill_pattern(i420, t);
            DrcFrame f;
            if (drc_encoder_encode(e, i420, t == 0, &f) != 0) {
                continue;
            }
            uint8_t idr_hdr[] = { 0x25, 0xb8, 0x04, 0xff };
            uint8_t p_hdr[]   = { 0x21, 0xe0, 0x03, 0xff };
            fwrite(start, sizeof(start), 1, dump);
            if (f.is_idr) {
                frame_number = 0;
                fwrite(idr_hdr, sizeof(idr_hdr), 1, dump);
            } else {
                frame_number = (frame_number + 1) & 0xFF;
                p_hdr[1] |= frame_number >> 3;
                p_hdr[2] |= frame_number << 5;
                fwrite(p_hdr, sizeof(p_hdr), 1, dump);
            }
            /* The emulation-prevention bytes H.264 requires, which the
             * packetiser's own path adds elsewhere. */
            size_t total = 0;
            for (int i = 0; i < DRC_ENC_CHUNKS; i++) total += f.size[i];
            uint8_t *flat = malloc(total), *esc = malloc(2 * total + 8);
            if (flat && esc) {
                size_t at = 0;
                for (int i = 0; i < DRC_ENC_CHUNKS; i++) {
                    memcpy(flat + at, f.chunk[i], f.size[i]);
                    at += f.size[i];
                }
                size_t n = 0;
                for (size_t i = 0; i < total; i++) {
                    if (i >= 2 && flat[i] <= 3 && !esc[n - 2] && !esc[n - 1]) {
                        esc[n++] = 3;
                    }
                    esc[n++] = flat[i];
                }
                fwrite(esc, n, 1, dump);
                written++;
            }
            free(flat);
            free(esc);
        }
        fclose(dump);
        t_eq_int("thirty frames were written", written, 30);

        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -v error -i %s -f null - 2>&1 | head -5", dump_path);
        FILE *pipe = popen(cmd, "r");
        char complaint[512] = {0};
        if (pipe) {
            size_t got = fread(complaint, 1, sizeof(complaint) - 1, pipe);
            complaint[got] = '\0';
            pclose(pipe);
        }
        if (complaint[0]) {
            printf("  ffmpeg says: %s", complaint);
        }
        t_ok("ffmpeg could not decode what this encoder produced",
             complaint[0] == '\0');
        remove(dump_path);
    }

    free(i420);
    drc_encoder_close(e);

    /* Closed cleanly, and gone from the process with it. */
    t_ok("drc-x264 is still globally visible after close", dlsym(RTLD_DEFAULT, "x264_encoder_open_140") == NULL);

    return t_report();
}
