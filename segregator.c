/*
 * Copyright (C) 2020 Samir Ibradzic
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cli_classic_usage(const char *name)
{
	printf("This program takes byte stream input from binary file and separates it\n"
	       "into two output files; one containing only even bytes from the input,\n"
	       "and another containing only odd bytes from the input.\n"
	        "It can also be used to reverse this process, see 'aggregate' option.\n\n");
	printf("Usage: %s [-h|"
	       "[-S -a <file> -e <output-evenfile> -o <output-oddfile> [-p <pad-block-size>]|\n"
	       "       %*s     "
	       " -A -e <evenfile> -o <oddfile> -a <output-aggrfile> [-u]]\n\n",
	       name, (int)strlen(name), " ");
	printf(" -h | --help                  print this help text\n"
	       " -S | --sergregate            read aggrfile and split into oddfile & evenfile.\n"
	       " -A | --aggregate             read oddfile & evenfile and aggregate it into aggrfile.\n"
	       " -a | --aggrfile <file>       aggregated file (can be both input or output).\n"
	       " -e | --evenfile <file>       file containing even bytes (can be both input or output).\n"
	       " -o | --oddfile <file>        file containing odd bytes (can be both input or output).\n"
	       " -p | --pad-block-size <KiB>  pad output files with 0xFF bytes to align to exact block size.\n"
	       " -u | --unpad                 remove trailing 0xFF bytes from the end of the aggregated file.\n\n");
}

static void cli_classic_abort_usage(const char *name)
{
	printf("Please run \"%s --help\" for usage info.\n", name);
	exit(1);
}

static int check_filename(char *filename, char *type)
{
	if (!filename || (filename[0] == '\0')) {
		fprintf(stderr, "Error: No %s file specified.\n", type);
		return 1;
	}
	/* Not an error, but maybe the user intended to specify a CLI option instead of a file name. */
	if (filename[0] == '-')
		fprintf(stderr, "Warning: Supplied %s file name starts with -\n", type);
	return 0;
}

int check_file_size(const char *filename, unsigned long *size)
{
	FILE *image;
	int ret = 0;
	if ((image = fopen(filename, "rb")) == NULL) {
		printf("Error: opening file \"%s\" failed: %s\n", filename, strerror(errno));
		return 1;
	}
	fseek(image, 0L, SEEK_END);
	*size = ftell(image);
	(void)fclose(image);
	return ret;
}

int read_buf_from_file(uint8_t *buf, const char *filename, unsigned long size)
{
	FILE *image;
	int ret = 0;
	if ((image = fopen(filename, "rb")) == NULL) {
		printf("Error: opening file \"%s\" failed: %s\n", filename, strerror(errno));
		return 1;
	}

	unsigned long numbytes = fread(buf, 1, size, image);
	if (numbytes != size) {
		printf("Error: Failed to read complete file. Got %ld bytes, "
			 "wanted %ld!\n", numbytes, size);
		ret = 1;
	}
	(void)fclose(image);
	return ret;
}

int write_buf_to_file(const uint8_t *buf, unsigned long size, const char *filename)
{
	FILE *image;
	int ret = 0;
	if (!filename) {
		printf("No filename specified.\n");
		return 1;
	}
	if ((image = fopen(filename, "wb")) == NULL) {
		printf("Error: opening file \"%s\" failed: %s\n", filename, strerror(errno));
		return 1;
	}
	unsigned long numbytes = fwrite(buf, 1, size, image);
	if (numbytes != size) {
		printf("Error: file %s could not be written completely.\n", filename);
		ret = 1;
		goto out;
	}
	if (fflush(image)) {
		printf("Error: flushing file \"%s\" failed: %s\n", filename, strerror(errno));
		ret = 1;
	}
out:
	if (fclose(image)) {
		printf("Error: closing file \"%s\" failed: %s\n", filename, strerror(errno));
		ret = 1;
	}
	return ret;
}

void segregate(const char *aggrfilename, const char *evenfilename,
	       const char *oddfilename, unsigned int blk_size)
{
	uint8_t *aggrcontents;
	uint8_t *evencontents;
	uint8_t *oddcontents;
	unsigned long aggrsize, split_size;
	unsigned long pad_size = 0;

	if (check_file_size(aggrfilename, &aggrsize))
		exit(1);

	split_size = aggrsize / 2;
	aggrcontents = malloc(aggrsize);

	if (aggrsize == 0) {
		printf("Error: file %s is empty.", aggrfilename);
		exit(1);
	}

	if (!(aggrsize % 2 == 0)) {
		printf("Error: file %s has odd number of bytes (%lu),"
		       "I can't segregte it evenly.\n", aggrfilename, aggrsize);
		exit(1);
	}

	if (blk_size)
		pad_size = blk_size*1024 - split_size%(blk_size*1024);

	if (read_buf_from_file(aggrcontents, aggrfilename, aggrsize))
		exit(1);

	evencontents = malloc(split_size + pad_size);
	oddcontents = malloc(split_size + pad_size);

	printf("Segregating %lu bytes long %s into %s & %s ...\n",
	       aggrsize, aggrfilename, evenfilename, oddfilename);
	for (unsigned long i = 0; i < aggrsize/2; i++) {
		evencontents[i] = aggrcontents[i*2];
		oddcontents[i] = aggrcontents[i*2+1];
	}

	if (blk_size) {
		printf("Padding output files with 0xFF's ...\n");
		memset(evencontents + split_size, 0xff, pad_size);
		memset(oddcontents + split_size, 0xff, pad_size);
	}

	write_buf_to_file(evencontents, split_size + pad_size, evenfilename);
	write_buf_to_file(oddcontents, split_size + pad_size, oddfilename);

	free(aggrcontents);
	free(evencontents);
	free(oddcontents);

	printf("Done.\n");
}

void aggregate(const char *evenfilename, const char *oddfilename,
	       const char *aggrfilename, unsigned int unpad_it)
{
	uint8_t *evencontents;
	uint8_t *oddcontents;
	uint8_t *aggrcontents;
	unsigned long evensize, oddsize;

	unsigned long p = 0;	// as in 'pointer' to byte in file image buffer

	if(check_file_size(evenfilename, &evensize) ||
	   check_file_size(oddfilename, &oddsize))
		exit(1);

	if (evensize == 0 || oddsize == 0) {
		printf("Error: No point in aggregating empty files.");
		exit(1);
	}

	if (evensize != oddsize) {
		printf("Error: files to be aggregated must be of same size.");
		exit(1);
	}

	evencontents = malloc(evensize);
	oddcontents = malloc(oddsize);
	aggrcontents = malloc(evensize*2);

	if (read_buf_from_file(evencontents, evenfilename, evensize))
		exit(1);
	if (read_buf_from_file(oddcontents, oddfilename, oddsize))
		exit(1);

	printf("Aggregating %lu bytes long %s and %s into %s ...\n",
	       evensize, evenfilename, oddfilename, aggrfilename);

	for (p = 0; p < evensize; p++) {
		aggrcontents[p*2] = evencontents[p];
		aggrcontents[p*2+1] = oddcontents[p];
	}

	if (unpad_it) {
		for (p = evensize*2-1; p > 0; --p) {
			if (aggrcontents[p] != 0xff) {
				p += 0x10 - (p % 0x10);	// align to 0x10 just in case
				printf("Cutting off 0xFF bytes from 0x%06lx\n", p);
				break;
			}
		}
	}

	write_buf_to_file(aggrcontents, p, aggrfilename);

	free(aggrcontents);
	free(evencontents);
	free(oddcontents);

	printf("Done.\n");
}

int main(int argc, char *argv[])
{
	int opt;
	int option_index = 0;

	static const char optstring[] = "hSAa:o:e:p:u";
	static const struct option long_options[] = {
		{"help",		0, NULL, 'h'},
		{"segregate",		0, NULL, 'S'},
		{"aggregate",		0, NULL, 'A'},
		{"aggrfile",		1, NULL, 'a'},
		{"evenfile",		1, NULL, 'e'},
		{"oddfile",		1, NULL, 'o'},
		{"pad-block-size",	1, NULL, 'p'},
		{"unpad",		0, NULL, 'u'},
		{NULL,			0, NULL, 0},
	};

	int segregate_it = 0, aggregate_it = 0, operation_specified = 0;
	unsigned int block_size = 0, unpad_it = 0;

	char *endptr = NULL;
	char *str_block_size = NULL;

	char *oddfilename = NULL;
	char *evenfilename = NULL;
	char *aggrfilename = NULL;

	setbuf(stdout, NULL);

	if (argc == 1) {
		cli_classic_abort_usage(argv[0]);
	}

	while ((opt = getopt_long(argc, argv, optstring,
				  long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'S':
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_classic_abort_usage(argv[0]);
			}
			segregate_it = 1;
			break;
		case 'A':
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_classic_abort_usage(argv[0]);
			}
			aggregate_it = 1;
			break;
		case 'a':
			aggrfilename = strdup(optarg);
			break;
		case 'e':
			evenfilename = strdup(optarg);
			break;
		case 'o':
			oddfilename = strdup(optarg);
			break;
		case 'p':
			str_block_size = strdup(optarg);
			block_size = strtoul(str_block_size, &endptr, 10);
			if(aggregate_it) {
				fprintf(stderr, "Padding option can not be used"
				        " when aggregating files. Aborting.\n");
				cli_classic_abort_usage(argv[0]);
			}
			if (*endptr != '\0' || endptr == str_block_size) {
				fprintf(stderr, "Invalid pad block size "
					"specified: %s Aborting.\n", str_block_size);
				cli_classic_abort_usage(argv[0]);
			}
			break;
		case 'u':
			if(segregate_it) {
				fprintf(stderr, "Un-pading can not be used "
				        "when segregating files. Aborting.\n");
				cli_classic_abort_usage(argv[0]);
			}
			unpad_it = 1;
			break;
		case 'h':
			if (++operation_specified > 1) {
				fprintf(stderr, "More than one operation "
					"specified. Aborting.\n");
				cli_classic_abort_usage(argv[0]);
			}
			cli_classic_usage(argv[0]);
			exit(0);
			break;
		default:
			cli_classic_abort_usage(argv[0]);
			break;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Error: Extra parameter found.\n");
		cli_classic_abort_usage(argv[0]);
	}

	if (segregate_it) {
		if(check_filename(aggrfilename, "aggregate file") ||
		   check_filename(evenfilename, "even bytes file") ||
		   check_filename(oddfilename, "odd bytes file"))
			cli_classic_abort_usage(argv[0]);
		segregate(aggrfilename, evenfilename, oddfilename, block_size);
	}

	if (aggregate_it) {
		if(check_filename(evenfilename, "even bytes file") ||
		   check_filename(oddfilename, "edd bytes file") ||
		   check_filename(aggrfilename, "aggregate file"))
			cli_classic_abort_usage(argv[0]);
		aggregate(evenfilename, oddfilename, aggrfilename, unpad_it);
	}
}
