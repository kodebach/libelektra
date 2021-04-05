/**
 * @file
 *
 * @brief Low level functions for access the Key Database.
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 */


#ifdef HAVE_KDBCONFIG_H
#include "kdbconfig.h"
#endif

#if DEBUG && defined(HAVE_STDIO_H)
#include <stdio.h>
#endif

#include <kdbassert.h>

#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#include <kdbinternal.h>


/**
 * @defgroup kdb KDB
 * @brief General methods to access the Key database.
 *
 * To use them:
 * @code
 * #include <kdb.h>
 * @endcode
 *
 * The kdb*() methods are used to access the storage, to get and set
 * @link keyset KeySets@endlink.
 *
 * Parameters common for all these functions are:
 *
 * - *handle*, as returned by kdbOpen(), need to be passed to every call
 * - *parentKey* is used for every call to add warnings and set an
 *   error. For kdbGet() / kdbSet() it is used to give an hint which keys
 *   should be retrieved/stored.
 *
 * @note The parentKey is an obligation for you, but only an hint for KDB.
 * KDB does not remember anything
 * about the configuration. You need to pass the same configuration
 * back to kdbSet(), otherwise parts of the configuration get
 * lost. Only keys below the parentKey are subject for change, the rest
 * must be left untouched.
 *
 * KDB uses different backend implementations that know the details
 * about how to access the storage.
 * One backend consists of multiple plugins.
 * See @link plugin writing a new plugin @endlink for information
 * about how to write a plugin.
 * Backends are state-less regarding the configuration (because of that
 * you must pass back the whole configuration for every backend), but
 * have a state for:
 *
 * - a two phase-commit
 * - a conflict detection (error C02000) and
 * - optimizations that avoid redoing already done operations.
 *
 * @image html state.png "State"
 * @image latex state.png "State"
 *
 * As we see in the figure, kdbOpen() can be called arbitrarily often in any
 * number of threads.
 *
 * For every handle you got from kdbOpen(), for every parentKey with a
 * different name, *only* the shown state transitions
 * are valid. From a freshly opened KDB, only kdbGet() and kdbClose()
 * are allowed, because otherwise conflicts (error C02000) would not be detected.
 *
 * Once kdbGet() was called (for a specific handle+parentKey),
 * any number of kdbGet() and kdbSet() can be
 * used with this handle respective parentKey, unless kdbSet() had
 * a conflict (error C02000) with another application.
 * Every affair with KDB needs to be finished with kdbClose().
 *
 * The name of the parentKey in kdbOpen() and kdbClose() does not matter.
 *
 * In the usual case we just have one parentKey and one handle. In
 * these cases we just have to remember to use kdbGet() before
 * kdbSet():
 *
 * @include kdbintro.c
 *
 * To output warnings, you can use following code:
 *
 * @snippet tests.c warnings
 *
 * To output the error, you can use following code:
 *
 * @snippet tests.c error
 *
 * @{
 */

static bool closeBackends (KeySet * backends, Key * errorKey)
{
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		const BackendData * backendData = keyValue (backendKey);

		for (elektraCursor p = 0; p < ksGetSize (backendData->plugins); p++)
		{
			Plugin * plugin = *(Plugin **) keyValue (ksAtCursor (backendData->plugins, p));
			if (elektraPluginClose (plugin, errorKey) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				return false;
			}
		}

		ksDel (backendData->plugins);
		ksDel (backendData->keys);
		ksDel (backendData->definition);
	}

	ksDel (backends);
	return true;
}

/**
 * @brief Takes the first key and cuts off this common part
 * for all other keys, instead name will be prepended
 *
 * @return a new allocated keyset with keys in user namespace.
 *
 * The first key is removed in the resulting keyset.
 */
KeySet * ksRenameKeys (KeySet * config, const char * name)
{
	Key * root;
	Key * cur;
	ssize_t rootSize = 0;

	ksRewind (config);

	root = ksNext (config);
	rootSize = keyGetNameSize (root);

	keyDel (ksLookup (config, root, KDB_O_POP));

	KeySet * newConfig = ksNew (ksGetSize (config), KS_END);
	if (rootSize == -1) return newConfig;

	while ((cur = ksPop (config)) != 0)
	{
		Key * dupKey = keyDup (cur, KEY_CP_ALL);
		keySetName (dupKey, name);
		keyAddName (dupKey, keyName (cur) + rootSize - 1);
		ksAppendKey (newConfig, dupKey);
		keyDel (cur);
	}

	return newConfig;
}

static void clearErrorAndWarnings (Key * key)
{
	Key * cutRoot = keyNew ("meta:/error", KEY_END);
	ksDel (ksCut (keyMeta (key), cutRoot));
	keySetName (cutRoot, "meta:/warnings");
	ksDel (ksCut (keyMeta (key), cutRoot));
	keyDel (cutRoot);
}

static bool ksKeyNeedSync (KeySet * ks)
{
	for (elektraCursor i = 0; i < ksGetSize (ks); i++)
	{
		if (keyNeedSync (ksAtCursor (ks, i)))
		{
			return true;
		}
	}
	return false;
}

/**
 * Checks whether the same instance of the list plugin is mounted in the global (maxonce) positions:
 *
 * pregetstorage, procgetstorage, postgetstorage, postgetcleanup,
 * presetstorage, presetcleanup, precommit, postcommit,
 * prerollback and postrollback
 *
 * @param handle the KDB handle to check
 * @param errorKey used for error reporting
 *
 * @retval 1 if list is mounted everywhere
 * @retval 0 otherwise
 */
static int ensureListPluginMountedEverywhere (KDB * handle, Key * errorKey)
{
	GlobalpluginPositions expectedPositions[] = { PREGETSTORAGE,
						      PROCGETSTORAGE,
						      POSTGETSTORAGE,
						      POSTGETCLEANUP,
						      PRESETSTORAGE,
						      PRESETCLEANUP,
						      PRECOMMIT,
						      POSTCOMMIT,
						      PREROLLBACK,
						      POSTROLLBACK,
						      -1 };

	Plugin * list = handle->globalPlugins[expectedPositions[0]][MAXONCE];
	if (list == NULL || elektraStrCmp (list->name, "list") != 0)
	{
		ELEKTRA_SET_INSTALLATION_ERRORF (errorKey, "list plugin not mounted at position %s/maxonce",
						 GlobalpluginPositionsStr[expectedPositions[0]]);
		return 0;
	}

	for (int i = 1; expectedPositions[i] > 0; ++i)
	{
		Plugin * plugin = handle->globalPlugins[expectedPositions[i]][MAXONCE];
		if (plugin != list)
		{
			// must always be the same instance
			ELEKTRA_SET_INSTALLATION_ERRORF (errorKey, "list plugin not mounted at position %s/maxonce",
							 GlobalpluginPositionsStr[expectedPositions[i]]);
			return 0;
		}
	}

	return 1;
}

/**
 * Handles the system:/elektra/contract/globalkeyset part of kdbOpen() contracts
 *
 * NOTE: @p contract will be modified
 *
 * @see kdbOpen()
 */
static void ensureContractGlobalKs (KDB * handle, KeySet * contract)
{
	Key * globalKsContractRoot = keyNew ("system:/elektra/contract/globalkeyset", KEY_END);
	Key * globalKsRoot = keyNew ("system:/elektra", KEY_END);

	KeySet * globalKs = ksCut (contract, globalKsContractRoot);

	ksRename (globalKs, globalKsContractRoot, globalKsRoot);

	ksAppend (handle->global, globalKs);

	ksDel (globalKs);
	keyDel (globalKsContractRoot);
	keyDel (globalKsRoot);
}

/**
 * Handles the system:/elektra/contract/mountglobal part of kdbOpen() contracts
 *
 * NOTE: @p contract will be modified
 *
 * @see kdbOpen()
 */
static int ensureContractMountGlobal (KDB * handle, KeySet * contract, Key * parentKey)
{
	if (!ensureListPluginMountedEverywhere (handle, parentKey))
	{
		return -1;
	}

	Plugin * listPlugin = handle->globalPlugins[PREGETSTORAGE][MAXONCE];
	typedef int (*mountPluginFun) (Plugin *, const char *, KeySet *, Key *);
	mountPluginFun listAddPlugin = (mountPluginFun) elektraPluginGetFunction (listPlugin, "mountplugin");
	typedef int (*unmountPluginFun) (Plugin *, const char *, Key *);
	unmountPluginFun listRemovePlugin = (unmountPluginFun) elektraPluginGetFunction (listPlugin, "unmountplugin");


	Key * mountContractRoot = keyNew ("system:/elektra/contract/mountglobal", KEY_END);
	Key * pluginConfigRoot = keyNew ("user:/", KEY_END);

	for (elektraCursor it = ksFindHierarchy (contract, mountContractRoot, NULL); it < ksGetSize (contract); it++)
	{
		Key * cur = ksAtCursor (contract, it);
		if (keyIsDirectlyBelow (mountContractRoot, cur) == 1)
		{
			const char * pluginName = keyBaseName (cur);
			KeySet * pluginConfig = ksCut (contract, cur);

			// increment ref count, because cur is part of pluginConfig and
			// we hold a reference to cur that is still needed (via pluginName)
			keyIncRef (cur);
			ksRename (pluginConfig, cur, pluginConfigRoot);

			int ret = listRemovePlugin (listPlugin, pluginName, parentKey);
			if (ret != ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				ret = listAddPlugin (listPlugin, pluginName, pluginConfig, parentKey);
			}

			// we ned to delete cur separately, because it was ksCut() from contract
			// we also need to decrement the ref count, because it was incremented above
			keyDecRef (cur);
			keyDel (cur);

			if (ret == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				ELEKTRA_SET_INSTALLATION_ERRORF (
					parentKey, "The plugin '%s' couldn't be mounted globally (via the 'list' plugin).", pluginName);
				return -1;
			}

			// adjust cursor, because we removed the current key
			--it;
		}
	}

	keyDel (mountContractRoot);
	keyDel (pluginConfigRoot);

	return 0;
}

/**
 * Handles the @p contract argument of kdbOpen().
 *
 * @see kdbOpen()
 */
static bool ensureContract (KDB * handle, const KeySet * contract, Key * parentKey)
{
	// TODO: tests
	// deep dup, so modifications to the keys in contract after kdbOpen() cannot modify the contract
	KeySet * dup = ksDeepDup (contract);

	ensureContractGlobalKs (handle, dup);
	int ret = ensureContractMountGlobal (handle, dup, parentKey);

	ksDel (dup);

	return ret == 0;
}

/**
 * @internal
 *
 * Helper for kdbOpen(). Creates empty KDB instance.
 *
 * @see kdbOpen()
 */
static KDB * kdbNew (Key * errorKey)
{
	KDB * handle = elektraCalloc (sizeof (struct _KDB));
	handle->modules = ksNew (0, KS_END);
	if (elektraModulesInit (handle->modules, errorKey) == -1)
	{
		// TODO (Q): shouldn't we let elektraModulesInit set this error?
		ELEKTRA_SET_INSTALLATION_ERROR (
			errorKey, "Method 'elektraModulesInit' returned with -1. See other warning or error messages for concrete details");

		ksDel (handle->modules);
		elektraFree (handle);
		return NULL;
	}
	handle->global =
		ksNew (1, keyNew ("system:/elektra/kdb", KEY_BINARY, KEY_SIZE, sizeof (handle), KEY_VALUE, &handle, KEY_END), KS_END);
	handle->backends = ksNew (0, KS_END);

	return handle;
}

static void addMountpoint (KeySet * backends, Key * mountpoint, Plugin * backend, KeySet * plugins, KeySet * definition)
{
	BackendData backendData = {
		.backend = backend,
		.keys = ksNew (0, KS_END),
		.plugins = plugins,
		.definition = definition,
		.initialized = false,
	};
	keySetBinary (mountpoint, &backendData, sizeof (backendData));
	ksAppendKey (backends, mountpoint);
}

static bool addElektraMountpoint (KeySet * backends, KeySet * modules, KeySet * global, Key * errorKey)
{
	// FIXME: replace KDB_DEFAULT_STORAGE with separate KDB_BOOTSTRAP_STORAGE
	Plugin * storage = elektraPluginOpen (KDB_DEFAULT_STORAGE, modules, ksNew (0, KS_END), errorKey);
	if (storage == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERRORF (errorKey, "Could not open boostrap storage plugin ('%s'). See warnings for details.",
						 KDB_DEFAULT_STORAGE);
		return false;
	}
	storage->global = global;

	// TODO (Q): support direct read-write to absolute path in backend plugin to avoid resolver in bootstrap?
	Plugin * resolver = elektraPluginOpen (KDB_DEFAULT_RESOLVER, modules, ksNew (0, KS_END), errorKey);
	if (resolver == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERRORF (errorKey, "Could not open boostrap resolver plugin ('%s'). See warnings for details.",
						 KDB_DEFAULT_RESOLVER);
		elektraPluginClose (resolver, errorKey);
		return false;
	}
	resolver->global = global;

	Plugin * backend = elektraPluginOpen ("backend", modules, ksNew (0, KS_END), errorKey);
	if (backend == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey,
						"Could not open system:/elektra backend during bootstrap. See other warnings for details");
		elektraPluginClose (resolver, errorKey);
		elektraPluginClose (storage, errorKey);
		return false;
	}
	backend->global = global;

	// clang-format off
	KeySet * plugins =
		ksNew (1,
			keyNew ("/#0", KEY_BINARY, KEY_SIZE, sizeof (resolver), KEY_VALUE, &resolver, KEY_END),
			keyNew ("/#1", KEY_BINARY, KEY_SIZE, sizeof (storage), KEY_VALUE, &storage, KEY_END),
		KS_END);
	KeySet * definition =
		ksNew (3,
			keyNew ("/path", KEY_VALUE, KDB_DB_INIT, KEY_END),
			keyNew ("/positions/get/resolver", KEY_VALUE, "#0", KEY_END),
			keyNew ("/positions/get/storage", KEY_VALUE, "#1", KEY_END),
			keyNew ("/positions/set/resolver", KEY_VALUE, "#0", KEY_END),
			keyNew ("/positions/set/storage", KEY_VALUE, "#1", KEY_END),
		KS_END);
	// clang-format on

	addMountpoint (backends, keyNew (KDB_SYSTEM_ELEKTRA, KEY_END), backend, plugins, definition);

	return true;
}

static KeySet * elektraBoostrap (KDB * handle, Key * errorKey)
{
	KeySet * elektraKs = ksNew (0, KS_END);
	Key * bootstrapParent = keyNew (KDB_SYSTEM_ELEKTRA, KEY_END);

	if (kdbGet (handle, elektraKs, bootstrapParent) == -1)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey,
						"Bootstrapping failed, please fix '" KDB_DB_SYSTEM "/" KDB_DB_INIT
						"'. If the error persists, please report this bug at https://issues.libelektra.org.");

		Key * warningsRoot = keyNew ("meta:/warnings", KEY_END);
		ksAppend (keyMeta (errorKey), ksBelow (keyMeta (bootstrapParent), warningsRoot));
		keyDel (warningsRoot);
		elektraTriggerWarnings (keyString (keyGetMeta (bootstrapParent, "meta:/error/number")), errorKey,
					keyString (keyGetMeta (bootstrapParent, "meta:/error/reason")));
		ksDel (elektraKs);
		keyDel (bootstrapParent);

		return NULL;
	}
	keyDel (bootstrapParent);

	return elektraKs;
}

static bool openPlugins (KeySet * plugins, KeySet * modules, KeySet * global, Key * errorKey)
{
	Key * pluginsRoot = keyNew ("/", KEY_END);
	bool success = true;
	for (elektraCursor i = 0; i < ksGetSize (plugins); i++)
	{
		Key * cur = ksAtCursor (plugins, i);
		if (keyIsDirectlyBelow (pluginsRoot, cur) == 1)
		{
			Key * lookupHelper = keyDup (cur, KEY_CP_NAME);
			keyAddBaseName (lookupHelper, "name");

			Key * nameKey = ksLookup (plugins, lookupHelper, 0);
			const char * pluginName = nameKey == NULL ? NULL : keyString (nameKey);
			if (nameKey == NULL || strlen (pluginName) == 0)
			{
				ELEKTRA_ADD_INSTALLATION_WARNINGF (errorKey,
								   "The plugin definition at '%s' doesn't contain a plugin name. Please "
								   "set '%s/name' to a non-empty string value.",
								   keyName (cur), keyName (cur));
				success = false;
				keyDel (lookupHelper);
				continue;
			}

			keySetBaseName (lookupHelper, "config");
			KeySet * config = ksBelow (plugins, lookupHelper);

			keyDel (lookupHelper);

			Plugin * plugin = elektraPluginOpen (pluginName, modules, config, errorKey);
			if (plugin == NULL)
			{
				ELEKTRA_ADD_INSTALLATION_WARNINGF (
					errorKey, "Could not open the plugin '%s' defined at '%s'. See other warnings for details.",
					pluginName, keyName (cur));
				success = false;
				continue;
			}
			plugin->global = global;

			// remove definition ...
			ksDel (ksCut (plugins, cur));
			// ... and replace with Plugin *
			Key * pluginKey = keyDup (cur, KEY_CP_NAME);
			keySetBinary (pluginKey, &plugin, sizeof (plugin));
			ksAppendKey (plugins, pluginKey);
		}
	}

	keyDel (pluginsRoot);
	return success;
}

static bool parseAndAddMountpoint (KeySet * mountpoints, KeySet * modules, KeySet * elektraKs, KeySet * global, Key * root, Key * errorKey)
{
	// check that the base name is a key name
	Key * mountpoint = keyNew (keyBaseName (root), KEY_END);
	if (mountpoint == NULL)
	{
		ELEKTRA_ADD_INSTALLATION_WARNINGF (errorKey, "'%s' is not a valid key name, but is used for the mountpoint '%s'",
						   keyBaseName (root), keyName (root));
		return false;
	}

	Key * elektraRoot = keyNew (KDB_SYSTEM_ELEKTRA, KEY_END);
	if (keyIsBelowOrSame (elektraRoot, mountpoint) != 0)
	{
		ELEKTRA_ADD_INSTALLATION_WARNINGF (
			errorKey,
			"The mountpoint '%s' (defined at '%s') is not allowed. Everything below '" KDB_SYSTEM_ELEKTRA
			"' is reserved for use by Elektra.",
			keyBaseName (root), keyName (root));
		return false;
	}

	// make a copy first and then cut/pop away the parts that don't belong
	KeySet * definition = ksBelow (elektraKs, root);

	// find backend reference
	Key * lookupHelper = keyDup (root, KEY_CP_NAME);
	keyAddBaseName (lookupHelper, "backend");
	Key * backendRef = ksLookup (definition, lookupHelper, KDB_O_POP);
	if (backendRef == NULL)
	{
		ELEKTRA_ADD_INSTALLATION_WARNINGF (errorKey, "The mountpoint '%s' defined in '%s' does not specify a backend plugin.",
						   keyName (mountpoint), keyName (root));
		keyDel (mountpoint);
		keyDel (lookupHelper);
		ksDel (definition);
		return false;
	}

	// get the plugin list and remove the common prefix
	keySetBaseName (lookupHelper, "plugins");
	KeySet * plugins = ksCut (definition, lookupHelper);
	Key * pluginsRoot = keyNew ("/", KEY_END);
	ksRename (plugins, lookupHelper, pluginsRoot);
	keyDel (pluginsRoot);

	// open all plugins (replaces key values with Plugin *)
	if (!openPlugins (plugins, modules, global, errorKey))
	{
		keyDel (mountpoint);
		keyDel (lookupHelper);
		keyDel (backendRef);
		ksDel (definition);
		ksDel (plugins);
		return false;
	}

	const char * backendIndex = keyString (backendRef);
	if (!elektraIsArrayPart (backendIndex))
	{
		ELEKTRA_ADD_INSTALLATION_WARNINGF (errorKey, "The value of '%s' is not a valid array index.", keyName (backendRef));
		keyDel (mountpoint);
		keyDel (lookupHelper);
		keyDel (backendRef);
		ksDel (definition);
		ksDel (plugins);
		return false;
	}

	keyAddBaseName (lookupHelper, backendIndex);
	Key * backendPluginKey = ksLookup (definition, lookupHelper, 0);
	if (backendRef == NULL)
	{
		ELEKTRA_ADD_INSTALLATION_WARNINGF (errorKey,
						   "The mountpoint '%s' defined in '%s' specifies '%s' as the index of the "
						   "backend plugin, but there is not such element in '%s/plugins'.",
						   keyName (mountpoint), keyName (root), backendIndex, keyName (root));
		keyDel (mountpoint);
		keyDel (lookupHelper);
		keyDel (backendRef);
		ksDel (definition);
		ksDel (plugins);
		return false;
	}

	Plugin * backendPlugin = (Plugin *) keyValue (backendPluginKey);
	addMountpoint (mountpoints, mountpoint, backendPlugin, plugins, definition);

	keyDel (lookupHelper);
	return true;
}

// FIXME: write tests
KeySet * elektraMountpointsParse (KeySet * elektraKs, KeySet * modules, KeySet * global, Key * errorKey)
{
	KeySet * mountpoints = ksNew (0, KS_END);

	Key * mountpointsRoot = keyNew (KDB_SYSTEM_ELEKTRA "/mountpoints", KEY_END);

	bool error = false;
	for (elektraCursor end, i = ksFindHierarchy (elektraKs, mountpointsRoot, &end); i < end;)
	{
		Key * cur = ksAtCursor (elektraKs, i);
		if (keyIsDirectlyBelow (mountpointsRoot, cur) == 1)
		{
			if (!parseAndAddMountpoint (mountpoints, modules, elektraKs, global, cur, errorKey))
			{
				error = true;
			}

			// skip over the keys we just parsed
			elektraCursor next;
			ksFindHierarchy (elektraKs, cur, &next);
			i = next - 1;
		}
		else
		{
			ELEKTRA_ADD_INSTALLATION_WARNINGF (
				errorKey,
				"The key '%s' is below 'system:/elektra/mountpoints', but doesn't belong to a mountpoint configuration. To "
				"define a mountpoint for the parent e.g. 'user:/mymountpoint' the key "
				"'system:/elektra/user:\\/mymountpoint' must exist and be set to an arbitrary (possibly empty) value.",
				keyName (cur));
			++i;
		}
	}

	if (error)
	{
		closeBackends (mountpoints, errorKey);
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Some mountpoints couldn't be parsed. See warnings for details.");
		return NULL;
	}

	return mountpoints;
}

static void addRootMountpoint (KeySet * backends, Plugin * plugin, KeySet * plugins, KeySet * definition, elektraNamespace ns)
{
	Key * rootKey = keyNew ("/", KEY_END);
	keySetNamespace (rootKey, ns);
	addMountpoint (backends, rootKey, plugin, plugins, definition);
}

static bool addModulesMountpoint (KDB * handle, Key * mountpoint, Key * errorKey)
{
	Plugin * modules = elektraPluginOpen ("modules", handle->modules, ksNew (0, KS_END), errorKey);
	if (modules == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERRORF (
			errorKey, "Could not open 'modules' plugin for mountpoint 'system:/elektra/modules/%s'. See warnings for details.",
			keyBaseName (mountpoint));
		return false;
	}

	Plugin * plugin = elektraPluginOpen (keyBaseName (mountpoint), handle->modules, ksNew (0, KS_END), errorKey);
	if (plugin == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERRORF (
			errorKey, "Could not open '%s' plugin for mountpoint 'system:/elektra/modules/%s'. See warnings for details.",
			keyBaseName (mountpoint), keyBaseName (mountpoint));
		return false;
	}
	modules->global = handle->global;
	addMountpoint (handle->backends, mountpoint, modules, ksNew (0, KS_END),
		       ksNew (1, keyNew ("/plugin", KEY_BINARY, KEY_SIZE, sizeof (plugin), KEY_VALUE, &plugin, KEY_END), KS_END));
	return true;
}

static bool addHardcodedMountpoints (KDB * handle, Key * errorKey)
{
	addElektraMountpoint (handle->backends, handle->modules, handle->global, errorKey);

	Plugin * defaultResolver = elektraPluginOpen (KDB_RESOLVER, handle->modules, ksNew (0, KS_END), errorKey);
	if (defaultResolver == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open default resolver plugin. See warnings for details.");
		return false;
	}
	defaultResolver->global = handle->global;

	Plugin * defaultStorage = elektraPluginOpen (KDB_STORAGE, handle->modules, ksNew (0, KS_END), errorKey);
	if (defaultStorage == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open default storage plugin. See warnings for details.");
		elektraPluginClose (defaultResolver, errorKey);
		return false;
	}
	defaultStorage->global = handle->global;

	Plugin * root = elektraPluginOpen ("backend", handle->modules, ksNew (0, KS_END), errorKey);
	if (root == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open default backend. See warnings for details.");
		elektraPluginClose (defaultResolver, errorKey);
		elektraPluginClose (defaultStorage, errorKey);
		return false;
	}
	root->global = handle->global;

	// clang-format off
	KeySet * rootPlugins =
		ksNew (2,
			keyNew ("/#0", KEY_BINARY, KEY_SIZE, sizeof (defaultResolver), KEY_VALUE, &defaultResolver, KEY_END),
			keyNew ("/#1", KEY_BINARY, KEY_SIZE, sizeof (defaultStorage), KEY_VALUE, &defaultStorage, KEY_END),
		KS_END);

	KeySet * rootDefinition =
		ksNew (7,
			keyNew ("/path", KEY_VALUE, KDB_DB_FILE, KEY_END),
			keyNew ("/positions/get/resolver", KEY_VALUE, "#0", KEY_END),
			keyNew ("/positions/get/storage", KEY_VALUE, "#1", KEY_END),
			keyNew ("/positions/set/resolver", KEY_VALUE, "#0", KEY_END),
			keyNew ("/positions/set/storage", KEY_VALUE, "#1", KEY_END),
			keyNew ("/positions/set/commit", KEY_VALUE, "#0", KEY_END),
			keyNew ("/positions/set/rollback", KEY_VALUE, "#0", KEY_END), 
		KS_END);
	// clang-format on

	addRootMountpoint (handle->backends, root, ksDup (rootPlugins), ksDup (rootDefinition), KEY_NS_SPEC);
	addRootMountpoint (handle->backends, root, ksDup (rootPlugins), ksDup (rootDefinition), KEY_NS_SYSTEM);
	addRootMountpoint (handle->backends, root, ksDup (rootPlugins), ksDup (rootDefinition), KEY_NS_USER);
	addRootMountpoint (handle->backends, root, ksDup (rootPlugins), ksDup (rootDefinition), KEY_NS_DIR);

	ksDel (rootPlugins);
	ksDel (rootDefinition);

	Key * modulesRoot = keyNew (KDB_SYSTEM_ELEKTRA "/modules", KEY_END);
	Plugin * modules = elektraPluginOpen ("modules", handle->modules, ksNew (0, KS_END), errorKey);
	if (modules == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open system:/elektra/modules backend. See warnings for details.");
		return false;
	}
	modules->global = handle->global;
	addMountpoint (handle->backends, modulesRoot, modules, ksNew (0, KS_END), ksNew (0, KS_END));

	for (elektraCursor i = 0; i < ksGetSize (handle->modules); i++)
	{
		Key * cur = ksAtCursor (handle->modules, i);
		if (keyIsDirectlyBelow (modulesRoot, cur) != 1)
		{
			continue;
		}

		if (!addModulesMountpoint (handle, keyDup (cur, KEY_CP_NAME), errorKey))
		{
			return false;
		}
	}

	Plugin * version = elektraPluginOpen ("version", handle->modules, ksNew (0, KS_END), errorKey);
	if (version == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open system:/elektra/version backend. See warnings for details.");
		return false;
	}
	version->global = handle->global;
	addMountpoint (handle->backends, keyNew (KDB_SYSTEM_ELEKTRA "/version", KEY_END), version, ksNew (0, KS_END), ksNew (0, KS_END));

	return true;
}

/**
 * @brief Opens the session with the Key database.
 *
 * @pre errorKey must be a valid key, e.g. created with keyNew()
 *
 * The method will bootstrap itself the following way.
 * The first step is to open the default backend. With it
 * system:/elektra/mountpoints will be loaded and all needed
 * libraries and mountpoints will be determined.
 * Then the global plugins and global keyset data from the @p contract
 * is processed.
 * Finally, the libraries for backends will be loaded and with it the
 * @p KDB data structure will be initialized.
 *
 * You must always call this method before retrieving or committing any
 * keys to the database. In the end of the program,
 * after using the key database, you must not forget to kdbClose().
 *
 * The pointer to the @p KDB structure returned will be initialized
 * like described above, and it must be passed along on any kdb*()
 * method your application calls.
 *
 * Get a @p KDB handle for every thread using elektra. Don't share the
 * handle across threads, and also not the pointer accessing it:
 *
 * @snippet kdbopen.c open
 *
 * You don't need kdbOpen() if you only want to
 * manipulate plain in-memory Key or KeySet objects.
 *
 * @pre errorKey must be a valid key, e.g. created with keyNew()
 *
 * @param contract the contract that should be ensured before opening the KDB
 *                 all data is copied and the KeySet can safely be used for
 *                 e.g. kdbGet() later
 * @param errorKey the key which holds errors and warnings which were issued
 * @see kdbGet(), kdbClose() to end all affairs to the key database.
 * @retval handle on success
 * @retval NULL on failure
 * @ingroup kdb
 */
KDB * kdbOpen (const KeySet * contract, Key * errorKey)
{
	if (!errorKey)
	{
		ELEKTRA_LOG ("no error key passed");
		return 0;
	}

	ELEKTRA_LOG ("called with %s", keyName (errorKey));
	Key * initialParent = keyDup (errorKey, KEY_CP_ALL);

	int errnosave = errno; // TODO (Q): really needed?

	// Step 1: create empty KDB instance
	KDB * handle = kdbNew (errorKey);
	if (handle == NULL)
	{
		goto error;
	}

	if (!addElektraMountpoint (handle->backends, handle->modules, handle->global, errorKey))
	{
		goto error;
	}

	// Step 3: execute bootstrap
	KeySet * elektraKs = elektraBoostrap (handle, errorKey);
	if (elektraKs == NULL)
	{
		goto error;
	}

	// Step 4: mount global plugins
	// TODO: remove/replace step in global plugins rewrite
	if (mountGlobals (handle, ksDup (elektraKs), handle->modules, errorKey) == -1)
	{
		// mountGlobals also sets a warning containing the name of the plugin that failed to load
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Mounting global plugins failed. Please see warning of concrete plugin");
		ksDel (elektraKs);
		goto error;
	}

	// Step 5: process contract
	if (contract != NULL && !ensureContract (handle, contract, errorKey))
	{
		ksDel (elektraKs);
		goto error;
	}

	// Step 6: parse mountpoints
	KeySet * backends = elektraMountpointsParse (elektraKs, handle->modules, handle->global, errorKey);
	if (backends == NULL)
	{
		ksDel (elektraKs);
		goto error;
	}

	// Step 7: switch from boostrap to real config
	ksDel (elektraKs);
	keyCopy (errorKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);

	if (!closeBackends (handle->backends, errorKey))
	{
		goto error;
	}

	handle->backends = backends;

	// Step 8: add hardcoded mountpoints
	if (!addHardcodedMountpoints (handle, errorKey))
	{
		goto error;
	}

	keyCopy (errorKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
	keyDel (initialParent);
	errno = errnosave;

	return handle;

error:
	if (handle != NULL)
	{
		Key * closeKey = keyNew ("/", KEY_END);
		kdbClose (handle, closeKey);
		keyDel (closeKey);
	}

	keyCopy (errorKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
	keyDel (initialParent);
	errno = errnosave;
	return NULL;
}


/**
 * Closes the session with the Key database.
 *
 * @pre The handle must be a valid handle as returned from kdbOpen()
 *
 * @pre errorKey must be a valid key, e.g. created with keyNew()
 *
 * This is the counterpart of kdbOpen().
 *
 * You must call this method when you finished your affairs with the key
 * database. You can manipulate Key and KeySet objects also after
 * kdbClose(), but you must not use any kdb*() call afterwards.
 *
 * The @p handle parameter will be finalized and all resources associated to it
 * will be freed. After a kdbClose(), the @p handle cannot be used anymore.
 *
 * @param handle contains internal information of
 *               @link kdbOpen() opened @endlink key database
 * @param errorKey the key which holds error/warning information
 * @retval 0 on success
 * @retval -1 on NULL pointer
 * @ingroup kdb
 */
int kdbClose (KDB * handle, Key * errorKey)
{
	if (!handle)
	{
		return -1;
	}

	Key * initialParent = keyDup (errorKey, KEY_CP_ALL);
	int errnosave = errno;
#if 1 == 0
	splitDel (handle->split);

	trieClose (handle->trie, errorKey);

	elektraPluginClose (handle->defaultBackend, errorKey);
	handle->defaultBackend = 0;

	// not set in fallback mode, so lets check:
	if (handle->initBackend)
	{
		elektraPluginClose (handle->initBackend, errorKey);
		handle->initBackend = 0;
	}
#endif
	if (handle->backends)
	{

		closeBackends (handle->backends, errorKey);
		handle->backends = NULL;
	}

	for (int i = 0; i < NR_GLOBAL_POSITIONS; ++i)
	{
		for (int j = 0; j < NR_GLOBAL_SUBPOSITIONS; ++j)
		{
			elektraPluginClose (handle->globalPlugins[i][j], errorKey);
		}
	}

	if (handle->modules)
	{
		elektraModulesClose (handle->modules, errorKey);
		ksDel (handle->modules);
	}
	else
	{
		ELEKTRA_ADD_RESOURCE_WARNING (errorKey, "Could not close modules: modules were not open");
	}

	if (handle->global)
	{
		ksDel (handle->global);
	}

	elektraFree (handle);

	keyCopy (errorKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
	keyDel (initialParent);
	errno = errnosave;
	return 0;
}

#if 2 == 0
static int elektraCacheCheckParent (KeySet * global, Key * cacheParent, Key * initialParent)
{
	const char * cacheName = keyGetNamespace (cacheParent) == KEY_NS_DEFAULT ? "" : keyName (cacheParent);

	// first check if parentkey matches
	Key * lastParentName = ksLookupByName (global, KDB_CACHE_PREFIX "/lastParentName", KDB_O_NONE);
	ELEKTRA_LOG_DEBUG ("LAST PARENT name: %s", keyString (lastParentName));
	ELEKTRA_LOG_DEBUG ("KDBG PARENT name: %s", cacheName);
	if (!lastParentName || elektraStrCmp (keyString (lastParentName), cacheName)) return -1;

	const char * cacheValue = keyGetNamespace (cacheParent) == KEY_NS_DEFAULT ? "default" : keyString (cacheParent);

	Key * lastParentValue = ksLookupByName (global, KDB_CACHE_PREFIX "/lastParentValue", KDB_O_NONE);
	ELEKTRA_LOG_DEBUG ("LAST PARENT value: %s", keyString (lastParentValue));
	ELEKTRA_LOG_DEBUG ("KDBG PARENT value: %s", cacheValue);
	if (!lastParentValue || elektraStrCmp (keyString (lastParentValue), cacheValue)) return -1;

	Key * lastInitalParentName = ksLookupByName (global, KDB_CACHE_PREFIX "/lastInitialParentName", KDB_O_NONE);
	Key * lastInitialParent = keyNew (keyString (lastInitalParentName), KEY_END);
	ELEKTRA_LOG_DEBUG ("LAST initial PARENT name: %s", keyName (lastInitialParent));
	ELEKTRA_LOG_DEBUG ("CURR initial PARENT name: %s", keyName (initialParent));

	if (!keyIsBelowOrSame (lastInitialParent, initialParent))
	{
		ELEKTRA_LOG_DEBUG ("CACHE initial PARENT: key is not below or same");
		keyDel (lastInitialParent);
		return -1;
	}

	keyDel (lastInitialParent);
	return 0;
}

static void elektraCacheCutMeta (KDB * handle)
{
	Key * parentKey = keyNew (KDB_CACHE_PREFIX, KEY_END);
	ksDel (ksCut (handle->global, parentKey));
	keyDel (parentKey);
}

KeySet * elektraCutProc (KeySet * ks)
{
	Key * parentKey = keyNew ("proc:/", KEY_END);
	KeySet * ret = ksCut (ks, parentKey);
	keyDel (parentKey);
	return ret;
}

static void elektraRestoreProc (KeySet * ks, KeySet * proc)
{
	ksAppend (ks, proc);
	ksDel (proc);
}

static void elektraCacheLoad (KDB * handle, KeySet * cache, Key * parentKey, Key * initialParent ELEKTRA_UNUSED, Key * cacheParent)
{
	// prune old cache info
	elektraCacheCutMeta (handle);

	if (elektraGlobalGet (handle, cache, cacheParent, PREGETCACHE, MAXONCE) != ELEKTRA_PLUGIN_STATUS_SUCCESS)
	{
		ELEKTRA_LOG_DEBUG ("CACHE MISS: could not fetch cache");
		elektraCacheCutMeta (handle);
		return;
	}
	ELEKTRA_ASSERT (elektraStrCmp (keyName (initialParent), keyName (parentKey)) == 0, "parentKey name differs from initial");
	if (elektraCacheCheckParent (handle->global, cacheParent, parentKey) != 0)
	{
		// parentKey in cache does not match, needs rebuild
		ELEKTRA_LOG_DEBUG ("CACHE WRONG PARENTKEY");
		elektraCacheCutMeta (handle);
		return;
	}
}

#ifdef ELEKTRA_ENABLE_OPTIMIZATIONS
/**
 * @brief Deletes the OPMPHM.
 *
 * Clears and frees all memory in Opmphm.
 *
 * @param opmphm the OPMPHM
 */
static void cacheOpmphmDel (Opmphm * opmphm)
{
	ELEKTRA_NOT_NULL (opmphm);
	if (opmphm && opmphm->size && !test_bit (opmphm->flags, OPMPHM_FLAG_MMAP_GRAPH))
	{
		elektraFree (opmphm->graph);
	}
	if (opmphm->rUniPar && !test_bit (opmphm->flags, OPMPHM_FLAG_MMAP_HASHFUNCTIONSEEDS))
	{
		elektraFree (opmphm->hashFunctionSeeds);
	}
	if (!test_bit (opmphm->flags, OPMPHM_FLAG_MMAP_STRUCT)) elektraFree (opmphm);
}

/**
 * @brief Deletes the OpmphmPredictor.
 *
 * Clears and frees all memory in OpmphmPredictor.
 *
 * @param op the OpmphmPredictor
 */
static void cacheOpmphmPredictorDel (OpmphmPredictor * op)
{
	ELEKTRA_NOT_NULL (op);
	if (!test_bit (op->flags, OPMPHM_PREDICTOR_FLAG_MMAP_PATTERNTABLE)) elektraFree (op->patternTable);
	if (!test_bit (op->flags, OPMPHM_PREDICTOR_FLAG_MMAP_STRUCT)) elektraFree (op);
}
#endif

static int elektraCacheLoadSplit (KDB * handle, Split * split, KeySet * ks, KeySet ** cache, Key ** cacheParent, Key * parentKey,
				  Key * initialParent, int debugGlobalPositions)
{
	ELEKTRA_LOG_DEBUG ("CACHE parentKey: %s, %s", keyName (*cacheParent), keyString (*cacheParent));

	if (splitCacheCheckState (split, handle->global) == -1)
	{
		ELEKTRA_LOG_DEBUG ("FAIL, have to discard cache because split state / SIZE FAIL, or file mismatch");
		elektraCacheCutMeta (handle);
		return -1;
	}

	ELEKTRA_LOG_DEBUG ("CACHE HIT");
	if (splitCacheLoadState (split, handle->global) != 0) return -1;

	if (debugGlobalPositions)
	{
		keySetName (parentKey, keyName (initialParent));
		elektraGlobalGet (handle, *cache, parentKey, PREGETSTORAGE, INIT);
		elektraGlobalGet (handle, *cache, parentKey, PREGETSTORAGE, MAXONCE);
		elektraGlobalGet (handle, *cache, parentKey, PREGETSTORAGE, DEINIT);
	}

	keySetName (parentKey, keyName (initialParent));
	// TODO: there are no error checks here, see kdbGet
	elektraGlobalGet (handle, *cache, parentKey, PROCGETSTORAGE, INIT);
	elektraGlobalGet (handle, *cache, parentKey, PROCGETSTORAGE, MAXONCE);
	elektraGlobalGet (handle, *cache, parentKey, PROCGETSTORAGE, DEINIT);

	// replace ks with cached keyset
	ksRewind (*cache);
	if (ks->size == 0)
	{
		ELEKTRA_LOG_DEBUG ("replacing keyset with cached keyset");
#ifdef ELEKTRA_ENABLE_OPTIMIZATIONS
		if (ks->opmphm) cacheOpmphmDel (ks->opmphm);
		if (ks->opmphmPredictor) cacheOpmphmPredictorDel (ks->opmphmPredictor);
#endif
		ksClose (ks);
		ks->array = (*cache)->array;
		ks->size = (*cache)->size;
		ks->alloc = (*cache)->alloc;
		ks->flags = (*cache)->flags;
#ifdef ELEKTRA_ENABLE_OPTIMIZATIONS
		ks->opmphm = (*cache)->opmphm;
		ks->opmphmPredictor = (*cache)->opmphmPredictor;
#endif
		elektraFree (*cache);
		*cache = 0;
	}
	else
	{
		ELEKTRA_LOG_DEBUG ("appending cached keyset (ks was not empty)");
		ksAppend (ks, *cache);
		ksDel (*cache);
		*cache = 0;
	}
	keyDel (*cacheParent);
	*cacheParent = 0;

	if (debugGlobalPositions)
	{
		keySetName (parentKey, keyName (initialParent));
		elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, INIT);
		elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, MAXONCE);
		elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, DEINIT);
	}

	return 0;
}
#endif

static bool initBackends (KeySet * backends, Key * parentKey)
{
	bool success = true;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		keySetMeta (backendKey, "meta:/internal/kdbreadonly", NULL);

		BackendData * backendData = (BackendData *) keyValue (backendKey);

		if (backendData->initialized)
		{
			// already initialized
			continue;
		}

		kdbInitPtr initFn = backendData->backend->kdbInit;
		if (initFn == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "The mountpoint '%s' defined a plugin ('%s') without a kdbInit function as a backend.",
				keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}

		// set up parentKey and global keyset
		keySetName (parentKey, KDB_SYSTEM_ELEKTRA "/mountpoints");
		keyAddBaseName (parentKey, keyName (backendKey));
		ksAppendKey (backendData->backend->global,
			     keyNew ("system:/elektra/kdb/backend/plugins", KEY_BINARY, KEY_SIZE, sizeof (backendData->plugins), KEY_VALUE,
				     &backendData->plugins, KEY_END));
		set_bit (parentKey->flags, KEY_FLAG_RO_NAME);

		int ret = initFn (backendData->backend, backendData->definition, parentKey);

		// restore parentKey
		clear_bit (parentKey->flags, KEY_FLAG_RO_NAME);

		// check return code
		switch (ret)
		{
		case ELEKTRA_PLUGIN_STATUS_SUCCESS:
			// successfully initialized
			backendData->initialized = true;
			break;
		case ELEKTRA_PLUGIN_STATUS_NO_UPDATE:
			// successfully initialized as read-only
			backendData->initialized = true;
			keySetMeta (backendKey, "meta:/internal/kdbreadonly", "1");
			break;
		case ELEKTRA_PLUGIN_STATUS_ERROR:
			// handle error
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "Calling the kdbInit function for the backend plugin ('%s') of the mountpoint '%s' has failed.",
				backendData->backend->name, keyName (backendKey));
			success = false;
			continue;
		default:
			// unknown result -> treat as error
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey,
				"The kdbInit function for the backend plugin ('%s') of the mountpoint '%s' returned "
				"an unknown result code '%d'. Treating the call as failed.",
				backendData->backend->name, keyName (backendKey), ret);
			success = false;
			continue;
		}
	}

	if (!success)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "The init phase of kdbGet() has failed. See warnings for details.");
	}

	return success;
}

static bool resolveBackendsForGet (KeySet * backends, Key * parentKey)
{
	bool success = true;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		keySetMeta (backendKey, "meta:/internal/kdbmountpoint", NULL);
		keySetMeta (backendKey, "meta:/internal/kdbneedsupdate", NULL);

		BackendData * backendData = (BackendData *) keyValue (backendKey);

		// check if get function exists
		// TODO (kodebach): move check into kdbOpen
		kdbGetPtr getFn = backendData->backend->kdbGet;
		if (getFn == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "The mountpoint '%s' defined a plugin ('%s') without a kdbGet function as a backend.",
				keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}

		// set up parentKey and global keyset for plugin
		keyCopy (parentKey, backendKey, KEY_CP_NAME);
		keySetString (parentKey, "");
		ksAppendKey (backendData->backend->global, keyNew ("system:/elektra/kdb/backend/phase", KEY_VALUE, "resolver", KEY_END));
		ksAppendKey (backendData->backend->global,
			     keyNew ("system:/elektra/kdb/backend/plugins", KEY_BINARY, KEY_SIZE, sizeof (backendData->plugins), KEY_VALUE,
				     &backendData->plugins, KEY_END));
		set_bit (parentKey->flags, KEY_FLAG_RO_NAME);

		int ret = getFn (backendData->backend, backendData->keys, parentKey);

		// restore parentKey
		clear_bit (parentKey->flags, KEY_FLAG_RO_NAME);

		// check return code
		switch (ret)
		{
		case ELEKTRA_PLUGIN_STATUS_SUCCESS:
			// Store returned mountpoint ID and mark for update
			keySetMeta (backendKey, "meta:/internal/kdbmountpoint", keyString (parentKey));
			keySetMeta (backendKey, "meta:/internal/kdbneedsupdate", "1");
			break;
		case ELEKTRA_PLUGIN_STATUS_NO_UPDATE:
			// no update needed
			keySetMeta (backendKey, "meta:/internal/kdbmountpoint", keyString (parentKey));
			break;
		case ELEKTRA_PLUGIN_STATUS_ERROR:
			// handle error
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"Calling the kdbGet function for the backend plugin ('%s') of the mountpoint '%s' "
							"has failed during the resolver phase.",
							backendData->backend->name, keyName (backendKey));
			success = false;
			continue;
		default:
			// unknown result -> treat as error
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey,
				"The kdbGet function for the backend plugin ('%s') of the mountpoint '%s' returned "
				"an unknown result code '%d' during the resolver phase. Treating the call as failed.",
				backendData->backend->name, keyName (backendKey), ret);
			success = false;
			continue;
		}
	}

	if (!success)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "The init phase of kdbGet() has failed. See warnings for details.");
	}

	return success;
}

static bool runGetPhase (KeySet * backends, Key * parentKey, const char * phase)
{
	bool success = true;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		BackendData * backendData = (BackendData *) keyValue (backendKey);

		// check if get function exists
		// TODO (kodebach): move check into kdbOpen
		kdbGetPtr getFn = backendData->backend->kdbGet;
		if (getFn == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "The mountpoint '%s' defined a plugin ('%s') without a kdbGet function as a backend.",
				keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}

		// set up parentKey and global keyset for plugin
		keyCopy (parentKey, backendKey, KEY_CP_NAME);
		keyCopy (parentKey, keyGetMeta (backendKey, "meta:/internal/kdbmountpoint"), KEY_CP_STRING);
		ksAppendKey (backendData->backend->global, keyNew ("system:/elektra/kdb/backend/phase", KEY_VALUE, phase, KEY_END));
		ksAppendKey (backendData->backend->global,
			     keyNew ("system:/elektra/kdb/backend/plugins", KEY_BINARY, KEY_SIZE, sizeof (backendData->plugins), KEY_VALUE,
				     &backendData->plugins, KEY_END));
		set_bit (parentKey->flags, KEY_FLAG_RO_NAME | KEY_FLAG_RO_VALUE);

		int ret = getFn (backendData->backend, backendData->keys, parentKey);

		// restore parentKey
		clear_bit (parentKey->flags, KEY_FLAG_RO_NAME | KEY_FLAG_RO_VALUE);

		// check return code
		switch (ret)
		{
		case ELEKTRA_PLUGIN_STATUS_SUCCESS:
		case ELEKTRA_PLUGIN_STATUS_NO_UPDATE:
			// success
			break;
		case ELEKTRA_PLUGIN_STATUS_ERROR:
			// handle error
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"Calling the kdbGet function for the backend plugin ('%s') of the mountpoint '%s' "
							"has failed during the %s phase.",
							backendData->backend->name, keyName (backendKey), phase);
			success = false;
			continue;
		default:
			// unknown result -> treat as error
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"The kdbGet function for the backend plugin ('%s') of the mountpoint '%s' returned "
							"an unknown result code '%d' during the %s phase. Treating the call as failed.",
							backendData->backend->name, keyName (backendKey), ret, phase);
			success = false;
			continue;
		}
	}

	if (!success)
	{
		ELEKTRA_SET_INTERFACE_ERRORF (parentKey, "The %s phase of kdbGet() has failed. See warnings for details.", phase);
	}

	return success;
}


/**
 * @brief Retrieve keys in an atomic and universal way.
 *
 * @pre The @p handle must be passed as returned from kdbOpen().
 *
 * @pre The @p returned KeySet must be a valid KeySet, e.g. constructed
 *     with ksNew().
 *
 * @pre The @p parentKey Key must be a valid Key, e.g. constructed with
 *     keyNew().
 *
 * If you pass NULL on any parameter kdbGet() will fail immediately without doing anything.
 *
 * The @p returned KeySet may already contain some keys, e.g. from previous
 * kdbGet() calls. The new retrieved keys will be appended using
 * ksAppendKey().
 *
 * If not done earlier kdbGet() will fully retrieve all keys under the @p parentKey
 * folder recursively (See Optimization below when it will not be done).
 *
 * @note kdbGet() might retrieve more keys than requested (that are not
 *     below parentKey). These keys must be passed to calls of kdbSet(),
 *     otherwise they will be lost. This stems from the fact that the
 *     user has the only copy of the whole configuration and backends
 *     only write configuration that was passed to them.
 *     For example, if you kdbGet() "system:/mountpoint/interest"
 *     you will not only get all keys below system:/mountpoint/interest,
 *     but also all keys below system:/mountpoint (if system:/mountpoint
 *     is a mountpoint as the name suggests, but
 *     system:/mountpoint/interest is not a mountpoint).
 *     Make sure to not touch or remove keys outside the keys of interest,
 *     because others may need them!
 *
 * @par Example:
 * This example demonstrates the typical usecase within an application
 * (without error handling).
 *
 * @include kdbget.c
 *
 * When a backend fails kdbGet() will return -1 with all
 * error and warning information in the @p parentKey.
 * The parameter @p returned will not be changed.
 *
 * @par Optimization:
 * In the first run of kdbGet all requested (or more) keys are retrieved. On subsequent
 * calls only the keys are retrieved where something was changed
 * inside the key database. The other keys stay in the
 * KeySet returned as passed.
 *
 * It is your responsibility to save the original keyset if you
 * need it afterwards.
 *
 * If you want to be sure to get a fresh keyset again, you need to open a
 * second handle to the key database using kdbOpen().
 *
 * @param handle contains internal information of @link kdbOpen() opened @endlink key database
 * @param parentKey is used to add warnings and set an error
 *         information. Additionally, its name is a hint which keys
 *         should be retrieved (it is possible that more are retrieved, see Note above).
 *           - cascading keys (starting with /) will retrieve the same path in all namespaces
 *           - / will retrieve all keys
 * @param ks the (pre-initialized) KeySet returned with all keys found
 * 	will not be changed on error or if no update is required
 * @see ksLookup(), ksLookupByName() for powerful
 * 	lookups after the KeySet was retrieved
 * @see kdbOpen() which needs to be called before
 * @see kdbSet() to save the configuration afterwards and kdbClose() to
 * 	finish affairs with the key database.
 * @retval 1 if the keys were retrieved successfully
 * @retval 0 if there was no update - no changes are made to the keyset then
 * @retval -1 on failure - no changes are made to the keyset then
 * @ingroup kdb
 */
int kdbGet (KDB * handle, KeySet * ks, Key * parentKey)
{
	// TODO (kodebach): different handling of namespaces
	// FIXME: write tests

	// Step 0: check preconditions
	if (parentKey == NULL)
	{
		ELEKTRA_LOG ("parentKey == NULL");
		return -1;
	}

	if (test_bit (parentKey->flags, KEY_FLAG_RO_META))
	{
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_META");
		return -1;
	}

	clearErrorAndWarnings (parentKey); // TODO (doc): NEW kdbGet now ALWAYS clears errors AND warnings

	if (test_bit (parentKey->flags, KEY_FLAG_RO_NAME))
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "parentKey with read-only name passed");
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_NAME");
		return -1;
	}

	if (test_bit (parentKey->flags, KEY_FLAG_RO_VALUE))
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "parentKey with read-only value passed");
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_VALUE");
		return -1;
	}

	if (keyGetNamespace (parentKey) == KEY_NS_META)
	{
		ELEKTRA_SET_INTERFACE_ERRORF (parentKey, "parentKey with meta:/ name passed ('%s')", keyName (parentKey));
		return -1;
	}

	if (handle == NULL)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "NULL pointer passed for handle");
		return -1;
	}

	if (ks == NULL)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "NULL pointer passed for KeySet");
		return -1;
	}

	int errnosave = errno;				      // TODO (Q): needed?
	Key * initialParent = keyDup (parentKey, KEY_CP_ALL); // TODO (kodebach): still needed with locks?

	ELEKTRA_LOG ("now in new kdbGet (%s)", keyName (parentKey));

#if 1 == 0
	Split * split = splitNew ();
#endif
	// Step 1: find backends for parentKey
	KeySet * backends = backendsForParentKey (handle->backends, parentKey);

	// Step 2: run init phase where needed
	if (!initBackends (backends, parentKey))
	{
		// TODO (kodebach): name not needed, once lock is in place
		keyCopy (parentKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
		keyDel (initialParent);

		errno = errnosave;
		return -1;
	}
	clear_bit (parentKey->flags, KEY_LOCK_NAME | KEY_LOCK_VALUE);

	// Step 3: run resolver phase
	if (!resolveBackendsForGet (backends, parentKey))
	{
		goto error;
	}

	// Step 4: remove up-to-date backends
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		if (keyGetMeta (ksAtCursor (backends, i), "meta:/internal/kdbneedsupdate") == NULL)
		{
			elektraKsPopAtCursor (backends, i);
			--i;
		}
	}

	// Step 5: return if no backends left
	if (ksGetSize (backends) == 0)
	{
		// TODO (kodebach): name not needed, once lock is in place
		keyCopy (parentKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
		keyDel (initialParent);

		keyCopy (parentKey, keyGetMeta (backendsFindParent (backends, parentKey), "meta:/internal/kdbmountpoint"), KEY_CP_STRING);

		ksDel (backends);
		errno = errnosave;
		return 0;
	}

	// check if cache is enabled, Steps 6-8 only run with cache
	// FIXME: implement
	bool cacheEnabled = false;
	if (cacheEnabled)
	{
		// Step 6: get cache entry IDs
		// FIXME: implement

		// Step 7: run cachecheck phase
		// FIXME: implement

		// Step 8: retrieve cache data
		// FIXME: implement
	}

	// Step 9a: run prestorage phase
	if (!runGetPhase (backends, parentKey, KDB_GET_PHASE_PRE_STORAGE))
	{
		goto error;
	}

	// Step 9b: discard data that plugins may have produced
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		const BackendData * backendData = keyValue (ksAtCursor (backends, i));
		ksClear (backendData->keys);
	}

	// Step 9c: run storage phase
	if (!runGetPhase (backends, parentKey, KDB_GET_PHASE_STORAGE))
	{
		goto error;
	}

	// Step 10: run poststorage phase for spec:/
	Key * specRoot = keyNew ("spec:/", KEY_END);
	if (!runGetPhase (ksBelow (backends, specRoot), parentKey, KDB_GET_PHASE_STORAGE))
	{
		keyDel (specRoot);
		goto error;
	}
	keyDel (specRoot);

	// Step 11: merge data from all backends
	KeySet * dataKs = ksNew (ksGetSize (ks), KS_END);
	backendsMerge (backends, dataKs);

	// Step 12: run procgetstorage global plugins
	if (elektraGlobalGet (handle, dataKs, parentKey, PROCGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}

	// Step 13: run postgetstorage global plugins
	if (elektraGlobalGet (handle, dataKs, parentKey, POSTGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}

	/* TODO: implement actual steps with new global plugins
		// Step 12: run gopts (if enabled)
		keyCopy (parentKey, initialParent, KEY_CP_NAME);
		keySetNamespace (cascadingParent, KEY_NS_CASCADING);
		set_bit (parentKey, KEY_LOCK_NAME | KEY_LOCK_VALUE);
		bool goptsEnabled = false;
		if (goptsEnabled && !goptsGet (dataKs, cascadingParent))
		{
			clear_bit (parentKey->flags, KEY_LOCK_NAME | KEY_LOCK_VALUE);
			goto error;
		}

		// Step 13: run spec plugin
		if (!specGet (dataKs, cascadingParent))
		{
			clear_bit (parentKey->flags, KEY_LOCK_NAME | KEY_LOCK_VALUE);
			goto error;
		}
		clear_bit (parentKey->flags, KEY_LOCK_NAME | KEY_LOCK_VALUE);
	*/

	// Step 14: split dataKs for poststorage phase
	if (!backendsDivide (backends, dataKs))
	{
		ELEKTRA_SET_INTERNAL_ERROR (parentKey,
					    "Couldn't divide keys into mountpoints before poststorage. Please report this bug at "
					    "https://issues.libelektra.org.");
		goto error;
	}

	// Step 15: run poststorage phase
	if (!runGetPhase (backends, parentKey, KDB_GET_PHASE_POST_STORAGE))
	{
		goto error;
	}

	// Step 16a: remove the parts of ks we read from backends
	// Note: we need to do this, so that in a second kdbGet() keys
	//       removed from the backend are removed from ks as well
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		ksDel (ksCut (ks, ksAtCursor (backends, i)));
	}

	// Step 16b: merge data into ks and return
	backendsMerge (backends, ks);

	// Step 17: update cache
	// FIXME: implement

	// TODO (kodebach): name not needed, once lock is in place
	keyCopy (parentKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
	keyDel (initialParent);

	keyCopy (parentKey, keyGetMeta (backendsFindParent (backends, parentKey), "meta:/internal/kdbmountpoint"), KEY_CP_STRING);

	ksDel (backends);
	errno = errnosave;
	return 1;

error:
	ELEKTRA_LOG_DEBUG ("now in error state");

	// TODO (kodebach): name not needed, once lock is in place
	keyCopy (parentKey, initialParent, KEY_CP_NAME);
	keyDel (initialParent);

	keyCopy (parentKey, keyGetMeta (backendsFindParent (backends, parentKey), "meta:/internal/kdbmountpoint"), KEY_CP_STRING);

	ksDel (backends);
	errno = errnosave;
	return -1;
}

static bool resolveBackendsForSet (KeySet * backends, Key * parentKey)
{
	bool success = true;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		keySetMeta (backendKey, "meta:/internal/kdbmountpoint", NULL);

		BackendData * backendData = (BackendData *) keyValue (backendKey);

		// check if set function exists
		// TODO (kodebach): move check into kdbOpen
		kdbSetPtr setFn = backendData->backend->kdbSet;
		if (setFn == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "The mountpoint '%s' defined a plugin ('%s') without a kdbSet function as a backend.",
				keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}

		// set up parentKey and global keyset for plugin
		keyCopy (parentKey, backendKey, KEY_CP_NAME);
		keySetString (parentKey, "");
		ksAppendKey (backendData->backend->global, keyNew ("system:/elektra/kdb/backend/phase", KEY_VALUE, "resolver", KEY_END));
		ksAppendKey (backendData->backend->global,
			     keyNew ("system:/elektra/kdb/backend/plugins", KEY_BINARY, KEY_SIZE, sizeof (backendData->plugins), KEY_VALUE,
				     &backendData->plugins, KEY_END));
		set_bit (parentKey->flags, KEY_FLAG_RO_NAME);

		int ret = setFn (backendData->backend, backendData->keys, parentKey);

		// restore parentKey
		clear_bit (parentKey->flags, KEY_FLAG_RO_NAME);

		// check return code
		switch (ret)
		{
		case ELEKTRA_PLUGIN_STATUS_NO_UPDATE:
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey,
				"Calling the kdbSet function for the backend plugin ('%s') of the mountpoint '%s' "
				"returned ELEKTRA_PLUGIN_STATUS_NO_UPDATE. This is interpreted the same way as "
				"ELEKTRA_PLUGIN_STATUS_SUCCESS, i.e. the mountpoint will still go through the rest of kdbSet()'s phases.",
				backendData->backend->name, keyName (backendKey));
			// FALLTRHOUGH
		case ELEKTRA_PLUGIN_STATUS_SUCCESS:
			// Store returned mountpoint ID and mark for update
			keySetMeta (backendKey, "meta:/internal/kdbmountpoint", keyString (parentKey));
			break;
		case ELEKTRA_PLUGIN_STATUS_ERROR:
			// handle error
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"Calling the kdbSet function for the backend plugin ('%s') of the mountpoint '%s' "
							"has failed during the resolver phase.",
							backendData->backend->name, keyName (backendKey));
			success = false;
			continue;
		default:
			// unknown result -> treat as error
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey,
				"The kdbSet function for the backend plugin ('%s') of the mountpoint '%s' returned "
				"an unknown result code '%d' during the resolver phase. Treating the call as failed.",
				backendData->backend->name, keyName (backendKey), ret);
			success = false;
			continue;
		}
	}

	if (!success)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "The init phase of kdbSet() has failed. See warnings for details.");
	}

	return success;
}

enum KdbSetFn
{
	KDB_SET_FN_SET,
	KDB_SET_FN_COMMIT,
	KDB_SET_FN_ERROR,
};

static bool runSetPhase (KeySet * backends, Key * parentKey, const char * phase, bool blockErrors, enum KdbSetFn function)
{
	if (blockErrors)
	{
		// set a dummy value to block errors
		// any errors that occur will be converted into warnings
		keySetMeta (parentKey, "error", "blocked");
	}

	bool success = true;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		BackendData * backendData = (BackendData *) keyValue (backendKey);

		// check if function exists
		// TODO (kodebach): move checks into kdbOpen
		if (function == KDB_SET_FN_SET && backendData->backend->kdbSet == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"The mountpoint '%s' defined a plugin ('%s') without a kdbSet function as a "
							"backend and the plugin didn't initialize the mountpoint as read-only.",
							keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}
		if (function == KDB_SET_FN_COMMIT && backendData->backend->kdbCommit == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"The mountpoint '%s' defined a plugin ('%s') without a kdbCommit function as a "
							"backend and the plugin didn't initialize the mountpoint as read-only.",
							keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}
		if (function == KDB_SET_FN_ERROR && backendData->backend->kdbError == NULL)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"The mountpoint '%s' defined a plugin ('%s') without a kdbError function as a "
							"backend and the plugin didn't initialize the mountpoint as read-only.",
							keyName (backendKey), backendData->backend->name);
			success = false;
			continue;
		}

		// set up parentKey and global keyset for plugin
		keyCopy (parentKey, backendKey, KEY_CP_NAME);
		keyCopy (parentKey, keyGetMeta (backendKey, "meta:/internal/kdbmountpoint"), KEY_CP_STRING);
		ksAppendKey (backendData->backend->global, keyNew ("system:/elektra/kdb/backend/phase", KEY_VALUE, phase, KEY_END));
		ksAppendKey (backendData->backend->global,
			     keyNew ("system:/elektra/kdb/backend/plugins", KEY_BINARY, KEY_SIZE, sizeof (backendData->plugins), KEY_VALUE,
				     &backendData->plugins, KEY_END));
		set_bit (parentKey->flags, KEY_FLAG_RO_NAME | KEY_FLAG_RO_VALUE);

		int ret;
		switch (function)
		{
		case KDB_SET_FN_SET:
			ret = backendData->backend->kdbSet (backendData->backend, backendData->keys, parentKey);
			break;
		case KDB_SET_FN_COMMIT:
			ret = backendData->backend->kdbCommit (backendData->backend, backendData->keys, parentKey);
			break;
		case KDB_SET_FN_ERROR:
			ret = backendData->backend->kdbError (backendData->backend, backendData->keys, parentKey);
			break;
		}

		// restore parentKey
		clear_bit (parentKey->flags, KEY_FLAG_RO_NAME | KEY_FLAG_RO_VALUE);

		// check return code
		switch (ret)
		{
		case ELEKTRA_PLUGIN_STATUS_SUCCESS:
		case ELEKTRA_PLUGIN_STATUS_NO_UPDATE:
			// success
			break;
		case ELEKTRA_PLUGIN_STATUS_ERROR:
			// handle error
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"Calling the kdbSet function for the backend plugin ('%s') of the mountpoint '%s' "
							"has failed during the %s phase.",
							backendData->backend->name, keyName (backendKey), phase);
			success = false;
			continue;
		default:
			// unknown result -> treat as error
			ELEKTRA_ADD_INTERFACE_WARNINGF (parentKey,
							"The kdbSet function for the backend plugin ('%s') of the mountpoint '%s' returned "
							"an unknown result code '%d' during the %s phase. Treating the call as failed.",
							backendData->backend->name, keyName (backendKey), ret, phase);
			success = false;
			continue;
		}
	}

	if (!success)
	{
		ELEKTRA_SET_INTERFACE_ERRORF (parentKey, "The %s phase of kdbSet() has failed. See warnings for details.", phase);
	}

	if (blockErrors)
	{
		// remove dummy error again
		keySetMeta (parentKey, "error", NULL);
		if (!success)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "Errors in %s are ignored. The error that occurred was converted into a warning.", phase);
		}
	}

	return success;
}

/** @brief Set keys in an atomic and universal way.
 *
 * @pre kdbGet() must be called before kdbSet():
 *    - initially (after kdbOpen())
 *    - after conflict errors in kdbSet().
 *
 * @pre The @p returned KeySet must be a valid KeySet, e.g. constructed
 *     with ksNew().
 *
 * @pre The @p parentKey Key must be a valid Key, e.g. constructed with
 *     keyNew(). It must not have read-only name, value or metadata.
 *
 * If you pass NULL on any parameter kdbSet() will fail immediately without doing anything.
 *
 * With @p parentKey you can give an hint which part of the given keyset
 * is of interest for you. Then you promise to only modify or
 * remove keys below this key. All others would be passed back
 * as they were retrieved by kdbGet().
 *
 * @par Errors
 * If `parentKey == NULL` or @p parentKey has read-only metadata, kdbSet() will
 * immediately return the error code -1. In all other error cases the following happens:
 * - kdbSet() will leave the KeySet's * internal cursor on the key that generated the error.
 * - Error information will be written into the metadata of
 *   the parent key, if possible.
 * - None of the keys are actually committed in this situation, i.e. no
 *   configuration file will be modified.
 *
 * In case of errors you should present the error message to the user and let the user decide what
 * to do. Possible solutions are:
 * - remove the problematic key and use kdbSet() again (for validation or type errors)
 * - change the value of the problematic key and use kdbSet() again (for validation errors)
 * - do a kdbGet() (for conflicts, i.e. error C02000) and then
 *   - set the same keyset again (in favour of what was set by this user)
 *   - drop the old keyset (in favour of what was set from another application)
 *   - merge the original, your own and the other keyset
 * - export the configuration into a file (for unresolvable errors)
 * - repeat the same kdbSet might be of limited use if the user does
 *   not explicitly request it, because temporary
 *   errors are rare and its unlikely that they fix themselves
 *   (e.g. disc full, permission problems)
 *
 * @par Optimization
 * Each key is checked with keyNeedSync() before being actually committed.
 * If no key of a backend needs to be synced
 * any affairs to backends are omitted and 0 is returned.
 *
 * @snippet kdbset.c set
 *
 * showElektraErrorDialog() and doElektraMerge() need to be implemented
 * by the user of Elektra. For doElektraMerge a 3-way merge algorithm exists in
 * libelektra-tools.
 *
 * @param handle contains internal information of @link kdbOpen() opened @endlink key database
 * @param ks a KeySet which should contain changed keys, otherwise nothing is done
 * @param parentKey is used to add warnings and set an error
 *         information. Additionally, its name is an hint which keys
 *         should be committed (it is possible that more are changed).
 *           - cascading keys (starting with /) will set the path in all namespaces
 *           - / will commit all keys
 *           - metanames will be rejected (error C01320)
 *           - empty/invalid (error C01320)
 * @retval 1 on success
 * @retval 0 if nothing had to be done, no changes in KDB
 * @retval -1 on failure, no changes in KDB, an error will be set on @p parentKey if possible (see "Errors" above)
 * @see keyNeedSync()
 * @see ksCurrent() contains the error key
 * @see kdbOpen() and kdbGet() that must be called first
 * @see kdbClose() that must be called afterwards
 * @ingroup kdb
 */
int kdbSet (KDB * handle, KeySet * ks, Key * parentKey)
{
	// TODO (kodebach): different handling of namespaces
	// FIXME: write tests

	// Step 0: check preconditions
	if (parentKey == NULL)
	{
		ELEKTRA_LOG ("parentKey == NULL");
		return -1;
	}

	if (test_bit (parentKey->flags, KEY_FLAG_RO_META))
	{
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_META");
		return -1;
	}

	clearErrorAndWarnings (parentKey); // TODO (doc): NEW kdbSet now ALWAYS clears errors AND warnings

	if (test_bit (parentKey->flags, KEY_FLAG_RO_NAME))
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "parentKey with read-only name passed");
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_NAME");
		return -1;
	}

	if (test_bit (parentKey->flags, KEY_FLAG_RO_VALUE))
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "parentKey with read-only value passed");
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_VALUE");
		return -1;
	}

	if (keyGetNamespace (parentKey) == KEY_NS_META)
	{
		ELEKTRA_SET_INTERFACE_ERRORF (parentKey, "parentKey with meta:/ name passed ('%s')", keyName (parentKey));
		return -1;
	}

	if (handle == NULL)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "NULL pointer passed for handle");
		return -1;
	}

	if (ks == NULL)
	{
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "NULL pointer passed for KeySet");
		return -1;
	}

	ELEKTRA_LOG ("now in new kdbSet (%s) %p %zd", keyName (parentKey), (void *) handle, ksGetSize (ks));

	// Step 1: check if ks has changed
	// FIXME: check that KS_FLAG_SYNC is properly set
	if (!ksNeedSync (ks))
	{
		// Step 2: check if any key in ks has changed
		if (!ksKeyNeedSync (ks))
		{
			// everything up-to-date -> return
			return 0;
		}
	}

	int errnosave = errno;				      // TODO (Q): needed?
	Key * initialParent = keyDup (parentKey, KEY_CP_ALL); // TODO (kodebach): still needed with locks?

	// Step 3: find backends for parentKey
	KeySet * backends = backendsForParentKey (handle->backends, parentKey);

	// Step 4: check that backends are initialized and remove read-only ones
	bool backendsInit = true;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);
		const BackendData * backendData = keyValue (backendKey);

		// check that backend is initialized
		if (!backendData->initialized)
		{
			ELEKTRA_ADD_INTERFACE_WARNINGF (
				parentKey, "The mountpoint '%s' has not been initialized. You need to call kdbGet() before kdbSet().",
				keyName (backendKey));
			backendsInit = false;
			continue;
		}

		// remove if read-only
		if (keyGetMeta (backendKey, "meta:/internal/kdbreadonly") != NULL)
		{
			elektraKsPopAtCursor (ks, i);
			--i;
		}
	}

	// Step 5: remove backends that haven't changed since kdbGet()
	// FIXME: implement

	if (!backendsInit)
	{
		ELEKTRA_SET_INTERFACE_ERROR (
			parentKey,
			"One or more mountpoints have not been initialized. Have you called kdbGet()? See warnings for details.");
		goto error;
	}

	// Step 6: run spec to add metadata
	// TODO: change once new global plugins are done
	if (elektraGlobalSet (handle, ks, parentKey, PRESETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}

	// Step 7: create deep-copy of ks
	// Note: This is needed so that ks retains its in-process state,
	//       after we transform the data into its on-disk state.
	KeySet * setKs = ksDeepDup (ks);

	// Step 8: split setKs for resolver and prestorage phases
	if (!backendsDivide (backends, setKs))
	{
		ELEKTRA_SET_INTERNAL_ERROR (parentKey,
					    "Couldn't divide keys into mountpoints at start of kdbSet. Please report this bug at "
					    "https://issues.libelektra.org.");
		goto error;
	}

	// Step 9a: resolve backends
	if (!resolveBackendsForSet (backends, parentKey))
	{
		goto rollback;
	}

	// Step 9b: run prestorage phase
	if (!runSetPhase (backends, parentKey, KDB_SET_PHASE_PRE_STORAGE, false, KDB_SET_FN_SET))
	{
		goto rollback;
	}

	/* TODO (kodebach): enable steps when spec is ready
		// Step 10: merge data from all backends (for spec removal)
		ksClear (setKs);
		backendsMerge (backends, setKs);

		// Step 11: run the spec plugin to remove copied metadata
		// FIXME: implement

		// Step 12: split setKs for remaining phases
		if (!backendsDivide (backends, setKs))
		{
			ELEKTRA_SET_INTERNAL_ERROR (parentKey, "Couldn't divide keys into mountpoints after spec removal. Please report this
	   bug at https://issues.libelektra.org."); goto rollback;
		}
	*/
	// Step 13a: run storage phase
	if (!runSetPhase (backends, parentKey, KDB_SET_PHASE_STORAGE, false, KDB_SET_FN_SET))
	{
		goto rollback;
	}

	// Step 13b: run poststorage phase
	if (!runSetPhase (backends, parentKey, KDB_SET_PHASE_POST_STORAGE, false, KDB_SET_FN_SET))
	{
		goto rollback;
	}

	// Step 14a: run precommit phase
	if (!runSetPhase (backends, parentKey, KDB_SET_PHASE_PRE_COMMIT, false, KDB_SET_FN_COMMIT))
	{
		goto rollback;
	}

	// Step 14b: run commit phase
	if (!runSetPhase (backends, parentKey, KDB_SET_PHASE_COMMIT, false, KDB_SET_FN_COMMIT))
	{
		goto rollback;
	}

	// Step 14c: run postcommit phase
	runSetPhase (backends, parentKey, KDB_SET_PHASE_POST_COMMIT, true, KDB_SET_FN_COMMIT);

	return 1;

rollback:
	// Step E1: run prerollback phase
	runSetPhase (backends, parentKey, KDB_SET_PHASE_PRE_ROLLBACK, true, KDB_SET_FN_ERROR);

	// Step E2: run rollback phase
	runSetPhase (backends, parentKey, KDB_SET_PHASE_ROLLBACK, true, KDB_SET_FN_ERROR);

	// Step E3: run postrollback phase
	runSetPhase (backends, parentKey, KDB_SET_PHASE_POST_ROLLBACK, true, KDB_SET_FN_ERROR);

error:
	// TODO (kodebach): name not needed, once lock is in place
	keyCopy (parentKey, initialParent, KEY_CP_NAME | KEY_CP_VALUE);
	keyDel (initialParent);
	errno = errnosave;

	return -1;
}

/**
 * @}
 */
