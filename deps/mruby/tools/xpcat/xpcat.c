#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
usage(const char *program)
{
  printf("Usage: %s -o outputfile FILE...\n", program);
}

int
main(int argc, char *argv[])
{
  int i, ch;
  const char *output = NULL;
  FILE *infile = NULL;
  FILE *outfile = NULL;

  if (argc < 4) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0) {
      i++;
      if (i < argc)
        output = argv[i];
      else {
        usage(argv[0]);
        return EXIT_FAILURE;
      }
    }
  }

  if (output) {
    outfile = fopen(output, "wb");
    if (!outfile) {
      fprintf(stderr, "[ERROR] unable to open output file: %s\n", output);
      return EXIT_FAILURE;
    }
    setbuf(outfile, NULL);

    for (i = 1; i < argc; i++) {
      if (strcmp(argv[i], "-o") == 0) {
        i++;
        continue;
      }

      infile = fopen(argv[i], "rb");
      if (!infile) {
        fprintf(stderr, "[ERROR] unable to open input file: %s\n", argv[i]);
        return EXIT_FAILURE;
      }
      setbuf(infile, NULL);

      while ((ch = getc(infile)) != EOF) {
        if (putc(ch, outfile) == EOF) {
          fprintf(stderr, "[ERROR] error writing output file: %s\n", output);
          return EXIT_FAILURE;
        }
      }

      fclose(infile);
    }
  }

  fclose(outfile);
  return EXIT_SUCCESS;
}

/* vim: set ts=2 sts=2 sw=2 et: */
