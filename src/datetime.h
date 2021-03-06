/* Plet
 * Copyright (c) 2021 Niels Sonnich Poulsen (http://nielssp.dk)
 * Licensed under the MIT license.
 * See the LICENSE file or http://opensource.org/licenses/MIT for more information.
 */

#ifndef DATETIME_H
#define DATETIME_H

#include "value.h"

void import_datetime(Env *env);

int rfc2822_date(time_t timestamp, Buffer *buffer);

#endif
