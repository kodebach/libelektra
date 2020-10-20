/**
 * @file
 *
 * @brief
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 */

#ifndef KDBMODULE_H
#define KDBMODULE_H

#include <kdb.h>
#include <kdbplugin.h>

#ifdef __cplusplus
namespace ckdb
{
extern "C" {
#endif

typedef Plugin * (*elektraPluginFactory) (void);
typedef void (*fn_t) (void);

int elektraModulesInit (KeySet * modules, Key * error);
fn_t elektraModulesLoad (KeySet * modules, const char * name, const char * symbol, Key * error);
int elektraModulesClose (KeySet * modules, Key * error);


#ifdef __cplusplus
}
}
#endif

#endif
