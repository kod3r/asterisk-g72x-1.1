#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <dlfcn.h>

#include <asterisk.h>
#include <asterisk/module.h>
#include <asterisk/translate.h>
#include <asterisk/options.h>
#include <asterisk/cli.h>

const struct ast_module_info *mod_info;
struct ast_translator *encoder, *decoder;
FILE *output;
const char *me;

__attribute__((noreturn)) static void usage(void)
{
    fprintf(stderr, "Usage: %s [-v] <codec_*.so> <-e|-d> <frame size> <input-file> <output-file>\n"
        "\t-v - verbose output (optional)\n"
        "\tcodec_*.so - path to codec file to load\n"
        "\t-e - encode SLIN (signed linear 16-bit audio) input-file into G.72x output-file\n"
        "\t-d - decode G.72x input-file into SLIN output-file\n"
        "\tframe size - use 160/10 for G.729a encoder/decoder, 480/24 for G.723.1\n"
        "\tinput/output-file - use '-' for stdin and stdout respectivelly\n"
        "Bugs to: http://groups.google.com/group/asterisk-g729\n", me);
    exit(1);
}

static FILE *open_file(char *name, char *mode, FILE *fallback)
{
    FILE *f;

    if (!strcmp(name, "-"))
        return fallback;
    if ((f = fopen(name, mode)) == NULL) {
        fprintf(stderr, "fopen(%s, %s) failed: %s\n", name, mode, strerror(errno));
        exit(4);
    }
    return f;
}

int main(int argc, char **argv)
{
    char *codec_filename, *op, *input_filename, *output_filename;
    int verbose = 0, i = 1, frame_size, encode;

    me = argv[0];
    if (argc != 6 && argc != 7)
        usage();
    if (argc == 7) {
        if (!strcmp(argv[1], "-v")) {
            verbose = 1;
            ++i;
        } else
            usage();
    }
    codec_filename = argv[i++];
    op = argv[i++];
    frame_size = atoi(argv[i++]);
    input_filename = argv[i++];
    output_filename = argv[i++];

    if (strlen(op) != 2 || op[0] != '-')
        usage();
    switch (op[1]) {
        case 'e': encode = 1; break;
        case 'd': encode = 0; break;
        default: usage();
    }

    void *mod = dlopen(codec_filename, RTLD_LAZY);
    if (mod == NULL) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", codec_filename, dlerror());
        exit(2);
    }

    mod_info->load();
    if (verbose)
        fprintf (stderr, "loaded encoder: %s; decoder: %s\n", encoder->name, decoder->name);

    FILE *input = open_file(input_filename, "r", stdin);
    output = open_file(output_filename, "w", stdout);

    const struct ast_translator *coder = encode ? encoder : decoder;
    struct ast_trans_pvt pvt;
    struct ast_frame f;
    memset(&pvt, 0, sizeof(pvt));
    memset(&f, 0, sizeof(f));
    pvt.pvt = malloc(coder->desc_size);
    pvt.outbuf.c = malloc(1600);
    if (coder->newpvt(&pvt)) {
        fprintf(stderr, "failed to initialize %s\n", coder->name);
        exit(3);
    }
    if (verbose)
        fprintf(stderr, "using %s\n", coder->name);

    int input_avail = 1, r = 0;
    f.frametype = AST_FRAME_VOICE;
    if (encode) {
        if (frame_size%2) {
            fprintf(stderr, "frame size must be multiple of 2, signed linear 16-bit audio format\n");
            exit(5);
        }
        short slin[frame_size/2];
        int padded = 0, frame_started = 0;

        f.subclass.codec = AST_FORMAT_SLINEAR;
        f.datalen = sizeof(slin);
        f.samples = sizeof(slin)/2;
        f.data.ptr = slin;
        while(1) {
            if (input_avail) {
                r = fread(slin, 1, sizeof(slin), input);
                if (r != sizeof(slin)) {
                    input_avail = 0;
                    if (ferror(input)) {
                        fprintf(stderr, "fread() failed: %s\n", strerror(errno));
                        fflush(output);
                        exit(4);
                    }
                }
            }
            if (r != sizeof(slin)) {
                if (!r && !frame_started)
                    break;
                padded += sizeof(slin) - r;
                memset((char*)&slin + r, 0, sizeof(slin) - r);
                r = 0;
            }
            coder->framein(&pvt, &f);
            frame_started = 1;
            if (coder->frameout) {
                if (coder->frameout(&pvt)) {
                    frame_started = 0;
                    if (!input_avail)
                        break;
                }
            } else {
                fprintf(stderr, "handling of encoder without frameout() routine is not implemented\n");
                exit(100);
            }
        }
        fflush(output);
        if (padded && verbose)
            fprintf(stderr, "input data is padded with %dms (%d bytes) of silence\n", padded/16, padded);
    /* decoder */
    } else {
        char bitstream[frame_size];

        f.subclass = coder->sample()->subclass;
        f.datalen = sizeof(bitstream);
        f.data.ptr = bitstream;
        while(1) {
            r = fread(bitstream, 1, sizeof(bitstream), input);
            if (r != sizeof(bitstream)) {
                if (ferror(input))
                    fprintf(stderr, "fread() failed: %s\n", strerror(errno));
                if (r != 0)
                    fprintf(stderr, "read truncated frame of size %d bytes\n", r);
                break;
            }
            coder->framein(&pvt, &f);
            if (coder->frameout) {
                fprintf(stderr, "handling of decoder with frameout() routine is not implemented\n");
                exit(100);
            }
            if (fwrite(pvt.outbuf.c, 1, pvt.datalen, output) != pvt.datalen) {
                fprintf(stderr, "fwrite() failed: %s\n", strerror(errno));
                fflush(output);
                exit(4);
            }
            pvt.datalen = pvt.samples = 0;
        }
        fflush(output);
    }

    coder->destroy(&pvt);

    return 0;
}

struct ast_frame *ast_trans_frameout(struct ast_trans_pvt *pvt, int datalen, int samples)
{
    if (fwrite(pvt->outbuf.c, 1, datalen /*? datalen : pvt->datalen */, output) != datalen) {
        fprintf(stderr, "fwrite() failed: %s\n", strerror(errno));
        fflush(output);
        exit(4);
    }
    pvt->datalen = pvt->samples = 0;
    return &pvt->f;
}

void ast_module_register(const struct ast_module_info *_mod_info)
{
    mod_info = _mod_info;
}

int __ast_register_translator(struct ast_translator *t, struct ast_module *module)
{
    if (t->srcfmt == AST_FORMAT_SLINEAR)
        encoder = t;
    else if (t->dstfmt == AST_FORMAT_SLINEAR)
        decoder = t;
    else {
        fprintf(stderr, "neither source nor destination codec format is SLIN\n");
        exit(3);
    }
    return 0;
}

/* unused but required to load codec module */
int option_verbose;
struct ast_flags ast_options;
void ast_module_unregister(const struct ast_module_info *_mod_info) {}
void ast_register_file_version(const char *file, const char *version) {}
void ast_unregister_file_version(const char *file) {}
/* G.723.1 defaults to 6.3kbps */
struct ast_config *ast_config_load2(const char *filename, const char *who_asked, struct ast_flags flags) { return NULL; }
int ast_cli_register(struct ast_cli_entry *e) { return 0; }
