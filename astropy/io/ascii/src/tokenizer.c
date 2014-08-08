// Licensed under a 3-clause BSD style license - see LICENSE.rst

#include "tokenizer.h"

tokenizer_t *create_tokenizer(char delimiter, char comment, char quotechar,
                              int fill_extra_cols, int strip_whitespace_lines,
                              int strip_whitespace_fields, int use_fast_converter)
{
    // Create the tokenizer in memory
    tokenizer_t *tokenizer = (tokenizer_t *) malloc(sizeof(tokenizer_t));

    // Initialize the tokenizer fields
    tokenizer->source = 0;
    tokenizer->source_len = 0;
    tokenizer->source_pos = 0;
    tokenizer->delimiter = delimiter;
    tokenizer->comment = comment;
    tokenizer->quotechar = quotechar;
    tokenizer->output_cols = 0;
    tokenizer->col_ptrs = 0;
    tokenizer->output_len = 0;
    tokenizer->num_cols = 0;
    tokenizer->num_rows = 0;
    tokenizer->fill_extra_cols = fill_extra_cols;
    tokenizer->state = START_LINE;
    tokenizer->code = NO_ERROR;
    tokenizer->iter_col = 0;
    tokenizer->curr_pos = 0;
    tokenizer->strip_whitespace_lines = strip_whitespace_lines;
    tokenizer->strip_whitespace_fields = strip_whitespace_fields;
    tokenizer->use_fast_converter = use_fast_converter;

    // This is a bit of a hack -- buf holds an empty string to represent
    // empty field values
    tokenizer->buf = calloc(2, sizeof(char));

    return tokenizer;
}

void delete_data(tokenizer_t *tokenizer)
{
    // Don't free tokenizer->source because it points to part of
    // an already freed Python object
    int i;

    if (tokenizer->output_cols)
	for (i = 0; i < tokenizer->num_cols; ++i)
	    free(tokenizer->output_cols[i]);
    
    free(tokenizer->output_cols);
    free(tokenizer->col_ptrs);
    free(tokenizer->output_len);

    // Set pointers to 0 so we don't use freed memory when reading over again
    tokenizer->output_cols = 0;
    tokenizer->col_ptrs = 0;
    tokenizer->output_len = 0;
}

void delete_tokenizer(tokenizer_t *tokenizer)
{
    delete_data(tokenizer);
    free(tokenizer->buf);
    free(tokenizer);
}

void resize_col(tokenizer_t *self, int index)
{
    // Temporarily store the position in output_cols[index] to
    // which col_ptrs[index] points
    int diff = self->col_ptrs[index] - self->output_cols[index];

    // Double the size of the column string
    self->output_cols[index] = (char *) realloc(self->output_cols[index], 2 * 
                                                self->output_len[index] * sizeof(char));

    // Set the second (newly allocated) half of the column string to all zeros
    memset(self->output_cols[index] + self->output_len[index] * sizeof(char), 0,
           self->output_len[index] * sizeof(char));

    self->output_len[index] *= 2;
    // realloc() might move the address in memory, so we have to move
    // col_ptrs[index] to an offset of the new address
    self->col_ptrs[index] = self->output_cols[index] + diff;
}

/*
  Resize the column string if necessary and then append c to the
  end of the column string, incrementing the column position pointer.
*/

#define PUSH(c)                                                 \
    if (self->col_ptrs[col] - self->output_cols[col] >=         \
        self->output_len[col])                                  \
    {                                                           \
        resize_col(self, col);                                  \
    }                                                           \
    *self->col_ptrs[col]++ = c;

/* Set the state to START_FIELD and begin with the assumption that
   the field is entirely whitespace in order to handle the possibility
   that the comment character is found before any non-whitespace even
   if whitespace stripping is disable.
*/

#define BEGIN_FIELD()                           \
    self->state = START_FIELD;                  \
    whitespace = 1;

/*
  First, backtrack to eliminate trailing whitespace if strip_whitespace_fields
  is true. If the field is empty, push '\x01' as a marker.
  Append a null byte to the end of the column string as a field delimiting marker.
  Increment the variable col if we are tokenizing data.
*/

#define END_FIELD()							\
    if (self->strip_whitespace_fields)                                  \
    {                                                                   \
        --self->col_ptrs[col];                                          \
        while (*self->col_ptrs[col] == ' ' || *self->col_ptrs[col] == '\t') \
        {                                                               \
            *self->col_ptrs[col]-- = '\x00';                            \
        }                                                               \
        ++self->col_ptrs[col];                                          \
    }                                                                   \
    if (self->col_ptrs[col] == self->output_cols[col] ||                \
        self->col_ptrs[col][-1] == '\x00')                              \
    {                                                                   \
        PUSH('\x01');                                                   \
    }                                                                   \
    PUSH('\x00');                                                       \
    if (!header)                                                        \
        ++col;

/*
  If we are tokenizing the header, end after the first line.
  Handle the possibility of insufficient columns appropriately;
  if fill_extra_cols=1, then append empty fields, but otherwise
  return an error. Increment our row count and possibly end if
  all the necessary rows have already been parsed.
*/

#define END_LINE()                              \
    if (header)                                 \
    {                                           \
        ++self->source_pos;                     \
        RETURN(NO_ERROR);                       \
    }                                           \
    else if (self->fill_extra_cols)             \
	while (col < self->num_cols)            \
	{                                       \
            PUSH('\x01');                       \
	    END_FIELD();                        \
	}                                       \
    else if (col < self->num_cols)              \
	RETURN(NOT_ENOUGH_COLS);                \
    ++self->num_rows;                           \
    old_state = START_LINE;                     \
    if (end != -1 && self->num_rows == end)     \
    {                                           \
        ++self->source_pos;                     \
        RETURN(NO_ERROR);                       \
    }

// Set the error code to c for later retrieval and return c
#define RETURN(c)                               \
    do {                                        \
        self->code = c;                         \
        return c;                               \
    } while (0)

#define HANDLE_CR() old_state = self->state; self->state = CARRIAGE_RETURN

int skip_lines(tokenizer_t *self, int offset, int header)
{
    int signif_chars = 0;
    int comment = 0;
    int i = 0;
    char c;
    
    while (i < offset)
    {
	if (self->source_pos >= self->source_len)
        {
            if (header)
                RETURN(INVALID_LINE); // header line is required
            else
                RETURN(NO_ERROR); // no data in input
        }

        c = self->source[self->source_pos];

        if (c == '\r' || c == '\n')
        {
            if (c == '\r' && self->source_pos < self->source_len - 1 &&
                self->source[self->source_pos + 1] == '\n')
            {
                ++self->source_pos; // skip \n in \r\n
            }
            if (!comment && signif_chars > 0)
                ++i;
            // Start by assuming a line is empty and non-commented
            signif_chars = 0;
            comment = 0;
        }

	else if ((c != ' ' && c != '\t') || !self->strip_whitespace_lines || header)
	{ 
            // comment line
            if (!signif_chars && self->comment != 0 && c == self->comment)
                comment = 1;

            // significant character encountered; during header
            // tokenization, we count whitespace unlike data tokenization
            // (see #2654)
            ++signif_chars;
	}

        ++self->source_pos;
    }

    RETURN(NO_ERROR);
}

int tokenize(tokenizer_t *self, int end, int header, int num_cols)
{
    delete_data(self); // clear old reading data
    char c; // input character
    int col = 0; // current column ignoring possibly excluded columns
    int old_state = START_LINE; // last state the tokenizer was in before CR mode
    int parse_newline = 0; // explicit flag to treat current char as a newline
    self->num_rows = 0;
    int i = 0;
    int whitespace = 1;

    if (header)
        self->num_cols = 1; // store header output in one column
    else
        self->num_cols = num_cols;

    // Allocate memory for structures used during tokenization
    self->output_cols = (char **) malloc(self->num_cols * sizeof(char *));
    self->col_ptrs = (char **) malloc(self->num_cols * sizeof(char *));
    self->output_len = (int *) malloc(self->num_cols * sizeof(int));
	
    for (i = 0; i < self->num_cols; ++i)
    {
        self->output_cols[i] = (char *) calloc(1, INITIAL_COL_SIZE *
                                               sizeof(char));
        // Make each col_ptrs pointer point to the beginning of the
        // column string
        self->col_ptrs[i] = self->output_cols[i];
        self->output_len[i] = INITIAL_COL_SIZE;
    }

    if (end == 0)
        RETURN(NO_ERROR); // don't read if end == 0

    self->state = START_LINE;

    // Loop until all of self->source has been read
    while (self->source_pos < self->source_len + 1)
    {
        if (self->source_pos == self->source_len || parse_newline)
            c = '\n';
        else
            c = self->source[self->source_pos];

        parse_newline = 0;

        switch (self->state)
        {
        case START_LINE:
            if (c == '\n')
                break;
            else if (c == '\r')
            {
                HANDLE_CR();
                break;
            }
            else if ((c == ' ' || c == '\t') && self->strip_whitespace_lines)
                break;
            else if (self->comment != 0 && c == self->comment)
            {
                // comment line; ignore
                self->state = COMMENT;
                break;
            }
            // initialize variables for the beginning of line parsing
            col = 0;
            BEGIN_FIELD();
            // parse in mode START_FIELD
	    
        case START_FIELD:
            // strip whitespace before field begins
            if ((c == ' ' || c == '\t') && self->strip_whitespace_fields)
                break;
            else if (!self->strip_whitespace_lines && self->comment != 0 &&
                     c == self->comment)
            {
                // comment line, not caught earlier because of no stripping
                self->state = COMMENT;
                break;
            }
            else if (c == self->delimiter) // field ends before it begins
            {
                END_FIELD();
                BEGIN_FIELD();
                break;
            }
            else if (c == '\r')
            {
                HANDLE_CR();
                break;
            }
            else if (c == '\n')
            {
                if (self->strip_whitespace_lines)
                {
                    // Move on if the delimiter is whitespace, e.g.
                    // '1 2 3   '->['1','2','3']
                    if (self->delimiter == ' ' || self->delimiter == '\t')
                        ;
                    // Register an empty field if non-whitespace delimiter,
                    // e.g. '1,2, '->['1','2','']
                    else
                    {
                        END_FIELD();
                    }                            
                }

                else if (!self->strip_whitespace_lines)
                {
                    // In this case we don't want to left-strip the field,
                    // so we backtrack
                    int tmp = self->source_pos;
                    --self->source_pos;

                    while (self->source_pos >= 0 &&
                           self->source[self->source_pos] != self->delimiter
                           && self->source[self->source_pos] != '\n'
                           && self->source[self->source_pos] != '\r')
                    {
                        --self->source_pos;
                    }

                    // backtracked to line beginning
                    if (self->source_pos == -1 || self->source[self->source_pos] == '\n'
                        || self->source[self->source_pos] == '\r')
                    {
                        self->source_pos = tmp;
                    }
                    else
                    {
                        ++self->source_pos;

                        if (self->source_pos == tmp)
                            // no whitespace, just an empty field
                            ;                                

                        else
                            while (self->source_pos < tmp)
                            {
                                // append whitespace characters
                                PUSH(self->source[self->source_pos]);
                                ++self->source_pos;
                            }

                        END_FIELD(); // whitespace counts as a field
                    }
                }
                        
                END_LINE();
                self->state = START_LINE;
                break;
            }
            else if (c == self->quotechar) // start parsing quoted field
            {
                self->state = START_QUOTED_FIELD;
                break;
            }
            else
            {
                if (col >= self->num_cols)
                    RETURN(TOO_MANY_COLS);
                // Valid field character, parse again in FIELD mode
                self->state = FIELD;
            }

        case FIELD:
            if (self->comment != 0 && c == self->comment && whitespace && col == 0)
            {
                // No whitespace stripping, but the comment char is found
                // before any data, e.g. '  # a b c'
                self->state = COMMENT;
            }
            else if (c == self->delimiter)
            {
                // End of field, look for new field
                END_FIELD();
                BEGIN_FIELD();
            }
            else if (c == '\r')
            {
                HANDLE_CR();
            }
            else if (c == '\n')
            {
                // Line ending, stop parsing both field and line
                END_FIELD();
                END_LINE();
                self->state = START_LINE;
            }
            else
            {
                if (c != ' ' && c != '\t')
                    whitespace = 0; // field is not all whitespace
                PUSH(c);
            }
            break;
		
        case START_QUOTED_FIELD:
            if ((c == ' ' || c == '\t') && self->strip_whitespace_fields)
            {
                // ignore initial whitespace
                break;
            }
            else if (c == self->quotechar) // empty quotes
            {
                END_FIELD();
                break;
            }
            else
            {
                // Valid field character, parse again in QUOTED_FIELD mode
                self->state = QUOTED_FIELD;
            }

        case QUOTED_FIELD_NEWLINE:
            if (self->state == QUOTED_FIELD)
                ; // fall through
            // Ignore initial whitespace if strip_whitespace_lines and
            // newlines regardless
            else if (((c == ' ' || c == '\t') && self->strip_whitespace_lines)
                     || c == '\n')
                break;
            else if (c == '\r')
            {
                HANDLE_CR();
                break;
            }
            else if (c == self->quotechar)
            {
                self->state = FIELD;
                break;
            }
            else
            {
                // Once data begins, parse it as a normal quoted field
                self->state = QUOTED_FIELD;
            }

        case QUOTED_FIELD:
            if (c == self->quotechar) // Parse rest of field normally, e.g. "ab"c
                self->state = FIELD;
            else if (c == '\n')
                self->state = QUOTED_FIELD_NEWLINE;
            else if (c == '\r')
            {
                HANDLE_CR();
            }
            else
            {
                PUSH(c);
            }
            break;
		
        case COMMENT:
            if (c == '\n')
                self->state = START_LINE;
            else if (c == '\r')
            {
                HANDLE_CR();
            }
            break; // keep looping until we find a newline

        case CARRIAGE_RETURN:
            self->state = old_state;
            --self->source_pos; // parse the newline in the old state
            if (c != '\n') // CR line terminator
            {
                --self->source_pos; // backtrack to the carriage return
                parse_newline = 1; // explicitly treat the CR as a newline
            }
            break;
        }

        ++self->source_pos;
    }
    
    RETURN(0);
}

long str_to_long(tokenizer_t *self, char *str)
{
    errno = 0;
    char *tmp;
    long ret = strtol(str, &tmp, 0);

    if (tmp == str || *tmp != '\0')
 	self->code = CONVERSION_ERROR;
    else if (errno == ERANGE)
        self->code = OVERFLOW_ERROR;

    return ret;
}

double str_to_double(tokenizer_t *self, char *str)
{
    char *tmp;
    double val;

    if (self->use_fast_converter)
    {
        val = xstrtod(str, &tmp, '.', 'E', ',', 1);

        if (*tmp)
            self->code = CONVERSION_ERROR;
        else if (errno == ERANGE)
            self->code = OVERFLOW_ERROR;
    }

    else
    {
        val = strtod(str, &tmp);

        if (tmp == str || *tmp != 0)
            self->code = CONVERSION_ERROR;
        else if (errno == ERANGE)
            self->code = OVERFLOW_ERROR;
    }

    return val;
}

// ---------------------------------------------------------------------------
// Implementation of xstrtod

//
// strtod.c
//
// Convert string to double
//
// Copyright (C) 2002 Michael Ringgaard. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the project nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//
// -----------------------------------------------------------------------
// Modifications by Warren Weckesser, March 2011:
// * Rename strtod() to xstrtod().
// * Added decimal and sci arguments.
// * Skip trailing spaces.
// * Commented out the other functions.
// Modifications by Richard T Guy, August 2013:
// * Add tsep argument for thousands separator
//

double xstrtod(const char *str, char **endptr, char decimal,
               char sci, char tsep, int skip_trailing)
{
    double number;
    int exponent;
    int negative;
    char *p = (char *) str;
    double p10;
    int n;
    int num_digits;
    int num_decimals;

    errno = 0;

    // Skip leading whitespace
    while (isspace(*p)) p++;

    // Handle optional sign
    negative = 0;
    switch (*p)
    {
    case '-': negative = 1; // Fall through to increment position
    case '+': p++;
    }

    number = 0.;
    exponent = 0;
    num_digits = 0;
    num_decimals = 0;

    // Process string of digits
    while (isdigit(*p))
    {
        number = number * 10. + (*p - '0');
        p++;
        num_digits++;

        p += (tsep != '\0' & *p == tsep);
    }

    // Process decimal part
    if (*p == decimal)
    {
        p++;

        while (isdigit(*p))
        {
            number = number * 10. + (*p - '0');
            p++;
            num_digits++;
            num_decimals++;
        }

        exponent -= num_decimals;
    }

    if (num_digits == 0)
    {
        errno = ERANGE;
        return 0.0;
    }

    // Correct for sign
    if (negative) number = -number;

    // Process an exponent string
    if (toupper(*p) == toupper(sci))
    {
        // Handle optional sign
        negative = 0;
        switch (*++p)
        {
        case '-': negative = 1;   // Fall through to increment pos
        case '+': p++;
        }

        // Process string of digits
        n = 0;
        while (isdigit(*p))
        {
            n = n * 10 + (*p - '0');
            p++;
        }

        if (negative)
            exponent -= n;
        else
            exponent += n;
    }


    if (exponent < DBL_MIN_EXP  || exponent > DBL_MAX_EXP)
    {

        errno = ERANGE;
        return HUGE_VAL;
    }

    // Scale the result
    p10 = 10.;
    n = exponent;
    if (n < 0) n = -n;
    while (n)
    {
        if (n & 1)
        {
            if (exponent < 0)
                number /= p10;
            else
                number *= p10;
        }
        n >>= 1;
        p10 *= p10;
    }


    if (number == HUGE_VAL) {
        errno = ERANGE;
    }

    if (skip_trailing) {
        // Skip trailing whitespace
        while (isspace(*p)) p++;
    }

    if (endptr) *endptr = p;


    return number;
}

void start_iteration(tokenizer_t *self, int col)
{
    // Begin looping over the column string with index col
    self->iter_col = col;
    // Start at the initial pointer position
    self->curr_pos = self->output_cols[col];
}

int finished_iteration(tokenizer_t *self)
{
    // Iteration is finished if we've exceeded the length of the column string
    // or the pointer is at an empty byte
    return (self->curr_pos - self->output_cols[self->iter_col] >=
            self->output_len[self->iter_col] * sizeof(char)
	    || *self->curr_pos == '\x00');
}

char *next_field(tokenizer_t *self, int *size)
{
    char *tmp = self->curr_pos;

    // pass through the entire field until reaching the delimiter
    while (*self->curr_pos != '\x00')
	++self->curr_pos;

    ++self->curr_pos; // next field begins after the delimiter

    if (*tmp == '\x01') // empty field; this is a hack
    {
        if (size)
            *size = 0;
        return self->buf;
    }

    else
    {
        if (size)
            *size = self->curr_pos - tmp - 1;
        return tmp;
    }
}

// memory mapping won't work on Windows

#if !defined(_WIN32)
#include <sys/mman.h>

memory_map *get_mmap(char *fname)
{
    memory_map *map = (memory_map *)malloc(sizeof(memory_map));
    map->file_ptr = fopen(fname, "r");

    if (!map->file_ptr) // file failed to open
    {
        free(map);
        return 0;
    }

    fseek(map->file_ptr, 0, SEEK_END);
    map->len = ftell(map->file_ptr);
    fseek(map->file_ptr, 0, SEEK_SET);
    map->ptr = (char *)mmap(0, map->len, PROT_READ, MAP_SHARED,
                            fileno(map->file_ptr), 0);
    if (map->ptr == MAP_FAILED)
    {
        fclose(map->file_ptr);
        free(map);
        return 0;
    }

    return map;
}

void free_mmap(memory_map *mmap)
{
    munmap(mmap->ptr, mmap->len);
    fclose(mmap->file_ptr);
    free(mmap);
}

#else
#include <windows.h>

memory_map *get_mmap(char *fname)
{
    memory_map *map = (memory_map *)malloc(sizeof(memory_map));
    map->file_ptr = CreateFile(fname, GENERIC_READ, 0, 0, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, 0);

    if (map->file_ptr == INVALID_HANDLE_VALUE)
    {
        free(map);
        return 0;
    }

    map->len = GetFileSize(map->file_ptr, 0);
    map->handle = CreateFileMapping(map->file_ptr, 0, PAGE_READONLY, 0, 0, 0);

    if (!map->handle)
    {
        CloseHandle(map->file_ptr);
        free(map);
        return 0;
    }

    map->ptr = MapViewOfFile(map->handle, FILE_MAP_READ, 0, 0, 0);

    if (!map->ptr)
    {
        CloseHandle(map->handle);
        CloseHandle(map->file_ptr);
        free(map);
        return 0;
    }

    return map;
}

void free_mmap(memory_map *mmap)
{
    CloseHandle(mmap->handle);
    CloseHandle(mmap->file_ptr);
    free(mmap);
}
#endif
