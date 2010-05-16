#ifndef _FILTERS_COMMON_H
#define _FILTERS_COMMON_H

char **split_string( char *string, char *sep, unsigned limit );
void free_string_array( char **array );

char **split_options( const char *opt_str, char *options[] );
char *get_option( const char *name, char **split_options );

#endif
