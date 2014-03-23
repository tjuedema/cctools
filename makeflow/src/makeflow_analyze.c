/*
Copyright (C) 2008- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

#include "cctools.h"
#include "catalog_query.h"
#include "create_dir.h"
#include "copy_stream.h"
#include "work_queue_catalog.h"
#include "datagram.h"
#include "disk_info.h"
#include "domain_name_cache.h"
#include "link.h"
#include "macros.h"
#include "hash_table.h"
#include "itable.h"
#include "debug.h"
#include "work_queue.h"
#include "work_queue_internal.h"
#include "delete_dir.h"
#include "stringtools.h"
#include "load_average.h"
#include "get_line.h"
#include "int_sizes.h"
#include "list.h"
#include "xxmalloc.h"
#include "getopt_aux.h"
#include "rmonitor.h"
#include "random_init.h"
#include "path.h"

#include "dag.h"
#include "visitors.h"

#include "makeflow_common.h"

/* Display options */
enum { SHOW_INPUT_FILES = 2,
       SHOW_OUTPUT_FILES,
       SHOW_MAKEFLOW_ANALYSIS,
       SHOW_DAG_FILE
};


#define MAKEFLOW_AUTO_WIDTH 1
#define MAKEFLOW_AUTO_GROUP 2

#define	MAKEFLOW_MIN_SPACE 10*1024*1024	/* 10 MB */

/* Unique integers for long options. */

enum { LONG_OPT_VERBOSE_PARSING, };

int verbose_parsing = 0;

void dag_show_analysis(struct dag *d)
{
	printf("num_of_tasks\t%d\n", itable_size(d->node_table));
	printf("depth\t%d\n", dag_depth(d));
	printf("width_uniform_task\t%d\n", dag_width_uniform_task(d));
	printf("width_guaranteed_max\t%d\n", dag_width_guaranteed_max(d));
}

void dag_show_input_files(struct dag *d)
{
	struct dag_file *f;
	struct list *il;

	il = dag_input_files(d);
	list_first_item(il);
	while((f = list_next_item(il)))
		printf("%s\n", f->filename);

	list_delete(il);
}

void collect_input_files(struct dag *d, char *bundle_dir, char *(*rename) (struct dag_node * d, const char *filename))
{
	char file_destination[PATH_MAX];
	char *new_name;

	struct list *il;
	il = dag_input_files(d);

	struct dag_file *f;

	if(!rename)
		rename = dag_node_translate_filename;

	list_first_item(il);
	while((f = list_next_item(il))) {
		new_name = rename(NULL, f->filename);
		char *dir = NULL;
		dir = xxstrdup(new_name);
		path_dirname(new_name, dir);
		if(dir){
			sprintf(file_destination, "%s/%s", bundle_dir, dir);
			if(!create_dir(file_destination, 0755)) {
				fprintf(stderr,  "Could not create %s. Check the permissions and try again.\n", file_destination);
				free(dir);
				exit(1);
			}
			free(dir);
		}

		sprintf(file_destination, "%s/%s", bundle_dir, new_name);
		fprintf(stdout, "%s\t%s\n", f->filename, new_name);
		free(new_name);
	}

	list_delete(il);
}

/* When a collision is detected (a file with an absolute path has the same base name as a relative file) a
 * counter is appended to the filename and the name translation is retried */
char *bundler_translate_name(const char *input_filename, int collision_counter)
{
	static struct hash_table *previous_names = NULL;
	static struct hash_table *reverse_names = NULL;
	if(!previous_names)
		previous_names = hash_table_create(0, NULL);
	if(!reverse_names)
		reverse_names = hash_table_create(0, NULL);

	char *filename = NULL;

	if(collision_counter){
		filename = string_format("%s%d", input_filename, collision_counter);
	}else{
		filename = xxstrdup(input_filename);
	}

	const char *new_filename;
	new_filename = hash_table_lookup(previous_names, filename);
	if(new_filename)
		return xxstrdup(new_filename);

	new_filename = hash_table_lookup(reverse_names, filename);
	if(new_filename) {
		collision_counter++;
		char *tmp = bundler_translate_name(filename, collision_counter);
		free(filename);
		return tmp;
	}
	if(filename[0] == '/') {
		new_filename = path_basename(filename);
		if(hash_table_lookup(previous_names, new_filename)) {
			collision_counter++;
			char *tmp = bundler_translate_name(filename, collision_counter);
			free(filename);
			return tmp;
		} else if(hash_table_lookup(reverse_names, new_filename)) {
			collision_counter++;
			char *tmp = bundler_translate_name(filename, collision_counter);
			free(filename);
			return tmp;
		} else {
			hash_table_insert(reverse_names, new_filename, filename);
			hash_table_insert(previous_names, filename, new_filename);
			return xxstrdup(new_filename);
		}
	} else {
		hash_table_insert(previous_names, filename, filename);
		hash_table_insert(reverse_names, filename, filename);
		return xxstrdup(filename);
	}
}

char *bundler_rename(struct dag_node *n, const char *filename)
{

	if(n) {
		struct list *input_files = dag_input_files(n->d);
		if(list_find(input_files, (int (*)(void *, const void *)) string_equal, (void *) filename)) {
			list_free(input_files);
			return xxstrdup(filename);
		}
	}
	return bundler_translate_name(filename, 0);	/* no collisions yet -> 0 */
}

void dag_show_output_files(struct dag *d)
{
	struct dag_file *f;
	char *filename;

	hash_table_firstkey(d->file_table);
	while(hash_table_nextkey(d->file_table, &filename, (void **) &f)) {
		if(f->target_of)
			fprintf(stdout, "%s\n", filename);
	}
}

static void show_help_analyze(const char *cmd)
{
	fprintf(stdout, "Use: %s [options] <dagfile>\n", cmd);
	fprintf(stdout, " %-30s Create portable bundle of workflow in <directory>\n", "-b,--bundle-dir=<directory>");
	fprintf(stdout, " %-30s Show this help screen.\n", "-h,--help");
	fprintf(stdout, " %-30s Show the pre-execution analysis of the Makeflow script - <dagfile>.\n", "-i,--analyze-exec");
	fprintf(stdout, " %-30s Show input files.\n", "-I,--show-input");
	fprintf(stdout, " %-30s Syntax check.\n", "-k,--syntax-check");
	fprintf(stdout, " %-30s Show output files.\n", "-O,--show-output");
	fprintf(stdout, " %-30s Show version string\n", "-v,--version");
}

int main(int argc, char *argv[])
{
	int c;
	random_init();
	set_makeflow_exe(argv[0]);
	debug_config(get_makeflow_exe());
	int display_mode = 0;

	cctools_version_debug(D_DEBUG, get_makeflow_exe());
	const char *dagfile;

	char *bundle_directory = NULL;
	int syntax_check = 0;

	struct option long_options_analyze[] = {
		{"bundle-dir", required_argument, 0, 'b'},
		{"help", no_argument, 0, 'h'},
		{"analyze-exec", no_argument, 0, 'i'},
		{"show-input", no_argument, 0, 'I'},
		{"syntax-check", no_argument, 0, 'k'},
		{"show-output", no_argument, 0, 'O'},
		{"version", no_argument, 0, 'v'},
		{0, 0, 0, 0}
	};
	const char *option_string_analyze = "b:hiIkOv";

	while((c = getopt_long(argc, argv, option_string_analyze, long_options_analyze, NULL)) >= 0) {
		switch (c) {
			case 'b':
				bundle_directory = xxstrdup(optarg);
				break;
			case 'h':
				show_help_analyze(get_makeflow_exe());
				return 0;
			case 'i':
				display_mode = SHOW_MAKEFLOW_ANALYSIS;
				break;
			case 'I':
				display_mode = SHOW_INPUT_FILES;
				break;
			case 'k':
				syntax_check = 1;
				break;
			case 'O':
				display_mode = SHOW_OUTPUT_FILES;
				break;
			case 'v':
				cctools_version_print(stdout, get_makeflow_exe());
				return 0;
			default:
				show_help_analyze(get_makeflow_exe());
				return 1;
		}
	}

	if((argc - optind) != 1) {
		int rv = access("./Makeflow", R_OK);
		if(rv < 0) {
			fprintf(stderr, "makeflow: No makeflow specified and file \"./Makeflow\" could not be found.\n");
			fprintf(stderr, "makeflow: Run \"%s -h\" for help with options.\n", get_makeflow_exe());
			return 1;
		}

		dagfile = "./Makeflow";
	} else {
		dagfile = argv[optind];
	}

	struct dag *d = dag_from_file(dagfile);
	if(!d) {
		fatal("makeflow: couldn't load %s: %s\n", dagfile, strerror(errno));
	}

	if(syntax_check) {
		fprintf(stdout, "%s: Syntax OK.\n", dagfile);
		return 0;
	}

	if(bundle_directory) {
		char expanded_path[PATH_MAX];

		collect_input_files(d, bundle_directory, bundler_rename);
		realpath(bundle_directory, expanded_path);

		char output_makeflow[PATH_MAX];
		sprintf(output_makeflow, "%s/%s", expanded_path, path_basename(dagfile));
		if(strcmp(bundle_directory, "*"))
			dag_to_file(d, output_makeflow, bundler_rename);
		free(bundle_directory);
		exit(0);
	}

	if(display_mode)
	{
		switch(display_mode)
		{
			case SHOW_INPUT_FILES:
				dag_show_input_files(d);
				break;

			case SHOW_OUTPUT_FILES:
				dag_show_output_files(d);
				break;

			case SHOW_MAKEFLOW_ANALYSIS:
				dag_show_analysis(d);
				break;

			default:
				fatal("Unknown display option.");
				break;
		}
		exit(0);
	}

	return 0;
}

/* vim: set noexpandtab tabstop=4: */
