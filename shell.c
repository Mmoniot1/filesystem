//By Monica Moniot
#include <readline/readline.h>
#include <readline/history.h>

#define FS_IMPLEMENTATION
#include "fs.h"



typedef struct String {
	uint size;
	char* ptr;
} String;

typedef struct Tokens {
	uint size;
	String* ptr;
} Tokens;


uint get_tokenize_mem_size(const String text) {
	uint tokens_size = 0;
	bool is_word = 0;
	for_each_lt(i, text.size) {
		char ch = text.ptr[i];
		if(ch == ' ' || ch == '\t' || ch == '\n') {
			if(is_word) {
				is_word = 0;
				tokens_size += 1;
			}
		} else if(!is_word) {
			is_word = 1;
		}
	}
	if(is_word) {
		tokens_size += 1;
	}
	return tokens_size*sizeof(String);
}

Tokens tokenize(String text, void* mem) {
	Tokens tokens = {0, cast(String*, mem)};
	bool is_word = 0;
	String* cur_word = tokens.ptr;
	for_each_lt(i, text.size) {
		char ch = text.ptr[i];
		if(ch == ' ' || ch == '\t' || ch == '\n') {
			if(is_word) {
				is_word = 0;
				cur_word->size = ptr_dist(cur_word->ptr, &text.ptr[i]);
				cur_word += 1;
				tokens.size += 1;
			}
		} else if(!is_word) {
			is_word = 1;
			cur_word->ptr = &text.ptr[i];
		}
	}
	if(is_word) {
		cur_word->size = ptr_dist(cur_word->ptr, &text.ptr[text.size]);
		tokens.size += 1;
	}
	return tokens;
}

int string_compare(const String str0, const String str1) {
	if(str0.size > str1.size) {
		return 1;
	} else if(str0.size < str1.size) {
		return -1;
	} else {
		return memcmp(str0.ptr, str1.ptr, str0.size);
	}
}


#define STATICSTR(name, size) static const String MACRO_CAT(str_, name) = {size, #name}
STATICSTR(q, 1);
STATICSTR(help, 4);
STATICSTR(newfs, 5);
STATICSTR(usefs, 5);
STATICSTR(closefs, 7);
STATICSTR(cd, 2);
STATICSTR(mkdir, 5);
STATICSTR(cat, 3);
STATICSTR(touch, 5);
STATICSTR(pipe, 4);
STATICSTR(ls, 2);
STATICSTR(home, 4);


int main(int argc, char** argv) {
    printf("Welcome! Enter help to get the list of available commands.\nYou can exit by entering q at any time.\n");
    bool is_running = 1;

	FS fs_mem;
	FS* fs = 0;
	File* cwd = 0;

    while(is_running) {
		char* buf = readline(">> ");
		if(!buf) continue;

		String input = {strlen(buf), buf};
        if(input.size == 0) {
			free(buf);
			continue;
		};
        add_history(buf);

		Tokens tokens = tokenize(input, malloc(get_tokenize_mem_size(input)));
		if(tokens.size == 0) {
			free(tokens.ptr);
			free(buf);
			continue;
		}
		String* cur_token = tokens.ptr;

		if(string_compare(*cur_token, str_q) == 0) {
			is_running = 0;
		} else if(string_compare(*cur_token, str_help) == 0) {
			printf("%s - quits the shell\n", str_q.ptr);
			printf("%s - lists all available commands\n", str_help.ptr);
			printf("%s - creates and uses a file system\n", str_newfs.ptr);
			printf("%s - looks for and uses an existing file system\n", str_usefs.ptr);
			printf("%s - closes the current file system\n", str_closefs.ptr);
			printf("%s - lists every file in the current working directory\n", str_ls.ptr);
			printf("%s - navigates into a new directory\n", str_cd.ptr);
			printf("%s - creates a new file\n", str_touch.ptr);
			printf("%s - writes data to a file\n", str_pipe.ptr);
			printf("%s - prints the contents of a file\n", str_cat.ptr);
			printf("%s - creates a new directory\n", str_mkdir.ptr);
			printf("%s - sets the current working directory to the root directory\n", str_home.ptr);
			printf("file paths are not implemented\n");
		} else if(string_compare(*cur_token, str_newfs) == 0) {
			if(tokens.size >= 3) {
				cur_token += 1;
				cur_token->ptr[cur_token->size] = 0;//NOTE: We're overwritting the spaces with null terminators
				char* device_name = cur_token->ptr;
				cur_token += 1;
				cur_token->ptr[cur_token->size] = 0;
				int capacity = atoi(cur_token->ptr);
				if(capacity >= MEGABYTE) {
					if(fs) {
						if(!fs_unmount(fs)) {
							fprintf(stderr, "error attempting to unmount for newfs\n");
						}
					}
					fs = &fs_mem;
					if(!fs_init(fs, device_name, capacity)) {
						fs = 0;
						fprintf(stderr, "an error occurred attempting to create fs\n");
					}
					cwd = fs_get_root(fs);
				} else {
					fprintf(stderr, "the entered capacity is too small; Minimum is %ld bytes\n", MEGABYTE);
				}
			} else {
				fprintf(stderr, "usage: newfs <filename> <capacity>\n");
			}
		} else if(string_compare(*cur_token, str_usefs) == 0) {
			if(tokens.size >= 2) {
				cur_token += 1;
				cur_token->ptr[cur_token->size] = 0;
				char* device_name = cur_token->ptr;

				if(fs) {
					if(!fs_unmount(fs)) {
						fprintf(stderr, "error attempting to unmount for usefs\n");
					}
				}
				fs = &fs_mem;
				if(!fs_mount(fs, device_name)) {
					fs = 0;
					fprintf(stderr, "an error occurred attempting to open fs\n");
				}
				cwd = fs_get_root(fs);
			} else {
				fprintf(stderr, "usage: usefs <filename>\n");
			}
		} else if(fs) {
			if(string_compare(*cur_token, str_closefs) == 0) {
				if(!fs_unmount(fs)) {
					fprintf(stderr, "error attempting to unmount fs\n");
				}
				fs = 0;
				cwd = 0;
			} else if(string_compare(*cur_token, str_cd) == 0) {
				if(tokens.size >= 2) {
					cur_token += 1;
					File* dir;
					if(fs_get_dir(fs, cwd, cur_token->ptr, cur_token->size, &dir)) {
						if(dir) {
							cwd = dir;
						} else {
							printf("the directory \"%.*s\" was not found\n", cur_token->size, cur_token->ptr);
						}
					} else {
						fprintf(stderr, "error attempting to cd to file\n");
					}
				} else {
					fprintf(stderr, "usage: cd <filename>\n");
				}
			} else if(string_compare(*cur_token, str_mkdir) == 0) {
				if(tokens.size >= 2) {
					cur_token += 1;
					File* dir;
					if(fs_open_dir(fs, cwd, cur_token->ptr, cur_token->size, &dir)) {
						if(!dir) {
							printf("the directory \"%.*s\" could not be created; the filename is already taken\n", cur_token->size, cur_token->ptr);
						}
					} else {
						fprintf(stderr, "error attempting to create directory\n");
					}
				} else {
					fprintf(stderr, "usage: mkdir <filename>\n");
				}
			} else if(string_compare(*cur_token, str_cat) == 0) {
				if(tokens.size >= 2) {
					cur_token += 1;
					File* file;
					if(fs_get_file(fs, cwd, cur_token->ptr, cur_token->size, &file)) {
						if(file) {
							uint64 size = fs_get_size(file);
							char* contents = (char*)malloc(size + 1);
							fs_read(fs, file, 0, contents, size);
							contents[size] = 0;
							printf("%s\n", contents);
							free(contents);
						} else {
							printf("the file \"%.*s\" was not found\n", cur_token->size, cur_token->ptr);
						}
					} else {
						fprintf(stderr, "error attempting to cat file\n");
					}
				} else {
					fprintf(stderr, "usage: cat <filename>\n");
				}
			} else if(string_compare(*cur_token, str_touch) == 0) {
				if(tokens.size == 2) {
					cur_token += 1;
					File* file;
					if(fs_open_file(fs, cwd, cur_token->ptr, cur_token->size, &file)) {
						if(!file) {
							printf("the file \"%.*s\" could not be created; the filename is already taken\n", cur_token->size, cur_token->ptr);
						}
					} else {
						fprintf(stderr, "error attempting to create file\n");
					}
				} else {
					fprintf(stderr, "usage: touch <filename>\n");
				}
			} else if(string_compare(*cur_token, str_pipe) == 0) {
				if(tokens.size >= 3) {
					cur_token += 1;
					char* filename = cur_token->ptr;
					uint filename_size = cur_token->size;
					cur_token += 1;
					File* file;
					if(fs_open_file(fs, cwd, filename, filename_size, &file)) {
						if(file) {
							if(!fs_write(fs, file, 0, cur_token->ptr, cur_token->size)) {
								fprintf(stderr, "error attempting to write to file\n");
							}
						} else {
							printf("the file \"%.*s\" could not be created; the filename is already taken\n", cur_token->size, cur_token->ptr);
						}
					} else {
						fprintf(stderr, "error attempting to create file\n");
					}
				} else {
					fprintf(stderr, "usage: pipe <filename> <data string>\n");
				}
			} else if(string_compare(*cur_token, str_ls) == 0) {
				File* file;
				if(fs_get_first_child(fs, cwd, &file)) {
					while(file) {
						uint size = fs_filename_size(file);
						char* name = (char*)malloc(size + 1);
						fs_get_filename(file, name);
						name[size] = 0;
						printf("%s\n", name);
						free(name);
						file = fs_get_next_child(fs, cwd, file);
					}
				} else {
					fprintf(stderr, "error attempting to read dir contents\n");
				}
			} else if(string_compare(*cur_token, str_home) == 0) {
				cwd = fs_get_root(fs);
			} else {
				fprintf(stderr, "unrecognized command\n");
			}
		} else {
			fprintf(stderr, "you must use newfs or usefs first\n");
		}

        // readline malloc's a new buffer every time.
		free(tokens.ptr);
        free(buf);
    }
	if(fs) {
		if(!fs_unmount(fs)) {
			fprintf(stderr, "error attempting to unmount fs\n");
		}
	}

    return 0;
}
