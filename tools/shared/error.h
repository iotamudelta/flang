/*
 * Copyright (c) 2015-2018, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/** \file
 * \brief Compiler Error handling and reporting.
 */

#ifndef COMPILER_ERROR_H_
#define COMPILER_ERROR_H_

/* Get the error_code enum which is generated by the errmsg util. */
#include "errmsgdf.h"

/* The rest of error definitions */
#include "flang/Error/pgerror.h"

/**
 * \brief Convert an integer to a string, for printing numbers in error
 * messages.
 *
 * This returns a pointer to static data, so only use it once.
 */
char *errnum(int num);

/**
   \brief ...
 */
int error_max_severity(void);

/**
   \brief ...
 */
int summary(bool final, int ipafollows);

/**
   \brief ...
 */
void asrt_failed(const char *filename, int line);

/**
   \brief ...
 */
void dassert_err(const char *filename, int line, const char *expr,
                 const char *txt);

/**
   \brief ...
 */
void erremit(int x);

/// \brief Issue a fatal error for gbl.lineno.
void errfatal(error_code_t ecode);

/// \brief Issue an informational message for gbl.lineno.
void errinfo(error_code_t ecode);

/**
   \brief ...
 */
void errini(void);

/// \brief Massage label name before calling error().
void errlabel(error_code_t ecode, enum error_severity sev, int eline, char *nm,
              char *op2);

/** \brief Construct and issue error message
 *
 * Construct error message and issue it to user terminal and to listing file
 * if appropriate.
 *
 * \param ecode:	error number
 * \param sev:	        error severity in range 1 ... 4
 * \param eline:	source file line number
 * \param op1:		strings to be expanded into error message * or 0
 * \param op2:		strings to be expanded into error message * or 0
 */
void error(error_code_t ecode, enum error_severity sev, int eline,
           const char *op1, const char *op2);

/// \brief Issue a severe error message for gbl.lineno.
void errsev(error_code_t ecode);

/**
   \brief ...
 */
void errversion(void);

/// \brief Issue a warning for gbl.lineno.
void errwarn(error_code_t ecode);

/**
   \brief ...
 */
void fperror(int errcode);

/** \brief Issue internal compiler error.
 *
 * \param txt:   null terminated text string identifying.
 * \param val:   integer value to be written with message.
 * \param sev:   error severity.
 */
void interr(const char *txt, int val, enum error_severity sev);

/** \brief Issue internal compiler error using printf-style formatting.
 *
 * \param sev:   error severity.
 * \param fmt:   printf-style format string
 * \param ...:   args for format string
 */
void interrf(enum error_severity sev, const char *fmt, ...);

#endif /* COMPILER_ERROR_H_ */
