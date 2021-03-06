/*
	run_avr.c

	Copyright 2008, 2010 Michel Pollet <buserror@gmail.com>

 	This file is part of simavr.

	simavr is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	simavr is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <libgen.h>
#include <string.h>
#include <signal.h>
#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_core.h"
#include "sim_gdb.h"
#include "sim_hex.h"
#include "sim_vcd_file.h"

#include "sim_core_decl.h"

static void
display_usage(
	const char * app)
{
	printf("Usage: %s [...] <firmware>\n", app);
	printf( "		[--freq|-f <freq>]  Sets the frequency for an .hex firmware\n"
			"		[--mcu|-m <device>] Sets the MCU type for an .hex firmware\n"
			"       [--list-cores]      List all supported AVR cores and exit\n"
			"       [--help|-h]         Display this usage message and exit\n"
			"       [--trace, -t]       Run full scale decoder trace\n"
			"       [-ti <vector>]      Add traces for IRQ vector <vector>\n"
			"       [--gdb|-g]          Listen for gdb connection on port 1234\n"
			"       [-ff <.hex file>]   Load next .hex file as flash\n"
			"       [-ee <.hex file>]   Load next .hex file as eeprom\n"
			"       [--input|-i <file>] A .vcd file to use as input signals\n"
			"       [--dump-vitals <file>]  Dump memory and cycle count to <file> on exit\n"
			"       [--max-cycles <n>]  Run for at most <n> cycles\n"
			"       [--max-instructions <n>]  Execute at most <n> instructions\n"
			"       [--exit-on-infinite]  End simulation when a vacuous infinite loop is reached\n"
			"       [-v]                Raise verbosity level\n"
			"                           (can be passed more than once)\n"
			"       <firmware>          A .hex or an ELF file. ELF files are\n"
			"                           prefered, and can include debugging syms\n");
	exit(1);
}

static void
list_cores()
{
	printf( "Supported AVR cores:\n");
	for (int i = 0; avr_kind[i]; i++) {
		printf("       ");
		for (int ti = 0; ti < 4 && avr_kind[i]->names[ti]; ti++)
			printf("%s ", avr_kind[i]->names[ti]);
		printf("\n");
	}
	exit(1);
}

static avr_t * avr = NULL;

static void
sig_int(
		int sign)
{
	printf("signal caught, simavr terminating\n");
	if (avr)
		avr_terminate(avr);
	exit(0);
}

int
main(
		int argc,
		char *argv[])
{
	elf_firmware_t f = {{0}};
	uint32_t f_cpu = 0;
	int trace = 0;
	int gdb = 0;
	int log = 1;
	char name[24] = "";
	uint32_t loadBase = AVR_SEGMENT_OFFSET_FLASH;
	int trace_vectors[8] = {0};
	int trace_vectors_count = 0;
	const char *vcd_input = NULL;
	
	const char* dump_vitals_filename = NULL;
	unsigned long long int max_cycles = 0;
	unsigned long long int max_instructions = 0;
	int exit_on_infinite_loop = 0;

	if (argc == 1)
		display_usage(basename(argv[0]));

	for (int pi = 1; pi < argc; pi++) {
		if (!strcmp(argv[pi], "--list-cores")) {
			list_cores();
		} else if (!strcmp(argv[pi], "-h") || !strcmp(argv[pi], "--help")) {
			display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "-m") || !strcmp(argv[pi], "--mcu")) {
			if (pi < argc-1)
				strncpy(name, argv[++pi], sizeof(name));
			else
				display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "-f") || !strcmp(argv[pi], "--freq")) {
			if (pi < argc-1)
				f_cpu = atoi(argv[++pi]);
			else
				display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "-i") || !strcmp(argv[pi], "--input")) {
			if (pi < argc-1)
				vcd_input = argv[++pi];
			else
				display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "-t") || !strcmp(argv[pi], "--trace")) {
			trace++;
		} else if (!strcmp(argv[pi], "-ti")) {
			if (pi < argc-1)
				trace_vectors[trace_vectors_count++] = atoi(argv[++pi]);
		} else if (!strcmp(argv[pi], "-g") || !strcmp(argv[pi], "--gdb")) {
			gdb++;
		} else if (!strcmp(argv[pi], "-v")) {
			log++;
		} else if (!strcmp(argv[pi], "-ee")) {
			loadBase = AVR_SEGMENT_OFFSET_EEPROM;
		} else if (!strcmp(argv[pi], "-ff")) {
			loadBase = AVR_SEGMENT_OFFSET_FLASH;
		} else if (!strcmp(argv[pi], "--dump-vitals")){
			if (pi < argc - 1)
				dump_vitals_filename = argv[++pi];
			else
				display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "--max-cycles")){
			if (pi < argc - 1)
				max_cycles = atoll(argv[++pi]);
			else
				display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "--max-instructions")){
			if (pi < argc - 1)
				max_instructions = atoll(argv[++pi]);
			else
				display_usage(basename(argv[0]));
		} else if (!strcmp(argv[pi], "--exit-on-infinite")){
			exit_on_infinite_loop = 1;
		} else if (argv[pi][0] != '-') {
			char * filename = argv[pi];
			char * suffix = strrchr(filename, '.');
			if (suffix && !strcasecmp(suffix, ".hex")) {
				if (!name[0] || !f_cpu) {
					fprintf(stderr, "%s: -mcu and -freq are mandatory to load .hex files\n", argv[0]);
					exit(1);
				}
				ihex_chunk_p chunk = NULL;
				int cnt = read_ihex_chunks(filename, &chunk);
				if (cnt <= 0) {
					fprintf(stderr, "%s: Unable to load IHEX file %s\n",
						argv[0], argv[pi]);
					exit(1);
				}
				printf("Loaded %d section of ihex\n", cnt);
				for (int ci = 0; ci < cnt; ci++) {
					if (chunk[ci].baseaddr < (1*1024*1024)) {
						f.flash = chunk[ci].data;
						f.flashsize = chunk[ci].size;
						f.flashbase = chunk[ci].baseaddr;
						printf("Load HEX flash %08x, %d\n", f.flashbase, f.flashsize);
					} else if (chunk[ci].baseaddr >= AVR_SEGMENT_OFFSET_EEPROM ||
							chunk[ci].baseaddr + loadBase >= AVR_SEGMENT_OFFSET_EEPROM) {
						// eeprom!
						f.eeprom = chunk[ci].data;
						f.eesize = chunk[ci].size;
						printf("Load HEX eeprom %08x, %d\n", chunk[ci].baseaddr, f.eesize);
					}
				}
			} else {
				if (elf_read_firmware(filename, &f) == -1) {
					fprintf(stderr, "%s: Unable to load firmware from file %s\n",
							argv[0], filename);
					exit(1);
				}
			}
		}
	}

	if (strlen(name))
		strcpy(f.mmcu, name);
	if (f_cpu)
		f.frequency = f_cpu;

	avr = avr_make_mcu_by_name(f.mmcu);
	if (!avr) {
		fprintf(stderr, "%s: AVR '%s' not known\n", argv[0], f.mmcu);
		exit(1);
	}
	avr_init(avr);
	avr->log = (log > LOG_TRACE ? LOG_TRACE : log);
	avr->trace = trace;
	avr_load_firmware(avr, &f);
	if (f.flashbase) {
		printf("Attempted to load a bootloader at %04x\n", f.flashbase);
		avr->pc = f.flashbase;
	}
	for (int ti = 0; ti < trace_vectors_count; ti++) {
		for (int vi = 0; vi < avr->interrupts.vector_count; vi++)
			if (avr->interrupts.vector[vi]->vector == trace_vectors[ti])
				avr->interrupts.vector[vi]->trace = 1;
	}
	if (vcd_input) {
		static avr_vcd_t input;
		if (avr_vcd_init_input(avr, vcd_input, &input)) {
			fprintf(stderr, "%s: Warning: VCD input file %s failed\n", argv[0], vcd_input);
		}
	}

	// even if not setup at startup, activate gdb if crashing
	avr->gdb_port = 1234;
	if (gdb) {
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
	}

	signal(SIGINT, sig_int);
	signal(SIGTERM, sig_int);

	unsigned long long int instruction_count = 0;
	int found_infinite_loop = 0;
	for (;;) {
		int old_pc = avr->pc;
		int state = avr_run(avr);
		instruction_count++;
		//If the --exit-on-infinite option was specified, terminate the simulation
		//if a jump-to-self instruction is encountered with interrupts disabled.
		if ( exit_on_infinite_loop && old_pc == avr->pc && !avr->sreg[S_I] ){
			found_infinite_loop = 1;
			state = cpu_Done;
		}
		//If the maximum number of cycles has been reached, terminate the simulation
		if (max_cycles && avr->cycle >= max_cycles){
			state = cpu_Done;
		}
		//If the maximum number of instructions has been reached, terminate the simulation
		if (max_instructions && instruction_count >= max_instructions){
			state = cpu_Done;
		}
		
		if (state == cpu_Done || state == cpu_Crashed)
			break;
	}
	
	if (dump_vitals_filename){
		FILE* dump_vitals_file = NULL;
		if (!strcmp(dump_vitals_filename,"-")){
			dump_vitals_file = stdout;
		}else{
			dump_vitals_file = fopen(dump_vitals_filename,"w");
		}
		
		fprintf(dump_vitals_file, "Cycle Count: %llu\n", (unsigned long long int)avr->cycle);
		fprintf(dump_vitals_file, "Instruction Count: %llu\n", instruction_count);
		fprintf(dump_vitals_file, "PC = 0x%08x\n",avr->pc);
		if (found_infinite_loop && exit_on_infinite_loop){
			fprintf(dump_vitals_file, "Infinite loop detected.\n");
		}
		
		//The avr->data array contains everything in data memory except the value of SREG, which is stored
		//separately. (Hooks in the load and store functions catch the cases where SREG is treated like a memory value).
		//This macro reassembles the SREG value into the appropriate memory location.
		READ_SREG_INTO(avr, avr->data[R_SREG]);
		
		//TODO programmatically find the size of data memory (0x2200 is only valid for the mega 2560)
		int i;
		fprintf(dump_vitals_file, "CONTENTS OF DATA MEMORY: ");
		for (i = 0; i < 0x2200; i++){
			fprintf(dump_vitals_file, "%02x ",(unsigned char)avr->data[i]);
		}
		fprintf(dump_vitals_file, "\n");
		
		if (!strcmp(dump_vitals_filename,"-")){
		
		}else{
			fclose(dump_vitals_file);
		}
	}

	avr_terminate(avr);
}
