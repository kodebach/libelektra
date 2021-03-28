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

		if (elektraPluginClose (backendData->backend, errorKey) != 0)
		{
			return false;
		}
		ksDel (backendData->keys);
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

static void addMountpoint (KeySet * backends, const char * mountpoint, Plugin * backend, KeySet * plugins, KeySet * definition)
{
	BackendData backendData = {
		.backend = backend,
		.keys = ksNew (0, KS_END),
		.plugins = plugins,
		.definition = definition,
	};
	ksAppendKey (backends, keyNew (mountpoint, KEY_BINARY, KEY_SIZE, sizeof (backendData), KEY_VALUE, &backendData, KEY_END));
}

static bool addElektraMountpoint (KeySet * backends, KeySet * modules, Key * errorKey)
{
	// FIXME: replace KDB_DEFAULT_STORAGE with separate KDB_BOOTSTRAP_STORAGE
	Plugin * storage = elektraPluginOpen (KDB_DEFAULT_STORAGE, modules, ksNew (0, KS_END), errorKey);
	if (storage == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERRORF (errorKey, "Could not open boostrap storage plugin ('%s'). See warnings for details.",
						 KDB_DEFAULT_STORAGE);
		return false;
	}

	Plugin * backend = elektraPluginOpen ("backend", modules, ksNew (0, KS_END), errorKey);
	if (backend == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey,
						"Could not open system:/elektra backend during bootstrap. See other warnings for details");
		elektraPluginClose (storage, errorKey);
		return false;
	}

	// clang-format off
	KeySet * plugins =
		ksNew (1,
			keyNew ("/#0", KEY_BINARY, KEY_SIZE, sizeof (storage), KEY_VALUE, storage, KEY_END),
		KS_END);
	KeySet * definition =
		ksNew (3,
			keyNew ("/path", KEY_VALUE, KDB_DB_SYSTEM "/" KDB_DB_INIT, KEY_END),
			keyNew ("/positions/get/storage", KEY_VALUE, "#0", KEY_END),
			keyNew ("/positions/set/storage", KEY_VALUE, "#0", KEY_END),
		KS_END);
	// clang-format on

	addMountpoint (backends, KDB_SYSTEM_ELEKTRA, backend, plugins, definition);

	return true;
}

static KeySet * elektraBoostrap (KDB * handle, Key * errorKey)
{
	KeySet * elektraKs = ksNew (0, KS_END);
	Key * bootstrapParent = keyNew (KDB_SYSTEM_ELEKTRA, KEY_END);

	int ret = kdbGet (handle, elektraKs, bootstrapParent);

	keyDel (bootstrapParent);

	if (ret == -1)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Bootstrapping failed, please fix '" KDB_DB_SYSTEM "/" KDB_DB_INIT "'");

		ksDel (elektraKs);
		return NULL;
	}

	return elektraKs;
}

static bool openPlugins (KeySet * plugins, KeySet * modules, Key * errorKey)
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

static bool parseAndAddMountpoint (KeySet * mountpoints, KeySet * modules, KeySet * elektraKs, Key * root, Key * errorKey)
{
	// check that the base name is a key name
	Key * mountpoint = keyNew (keyBaseName (root), KEY_END);
	if (mountpoint == NULL)
	{
		ELEKTRA_ADD_INSTALLATION_WARNINGF (errorKey, "'%s' is not a valid key name, but is used for the mountpoint '%s'",
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
	if (!openPlugins (plugins, modules, errorKey))
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
	addMountpoint (mountpoints, keyName (mountpoint), backendPlugin, plugins, definition);

	keyDel (lookupHelper);
	return true;
}

// FIXME: write tests
KeySet * elektraMountpointsParse (KeySet * elektraKs, KeySet * modules, Key * errorKey)
{
	KeySet * mountpoints = ksNew (0, KS_END);

	Key * mountpointsRoot = keyNew (KDB_SYSTEM_ELEKTRA "/mountpoints", KEY_END);

	bool error = false;
	for (elektraCursor end, i = ksFindHierarchy (elektraKs, mountpointsRoot, &end); i < end;)
	{
		Key * cur = ksAtCursor (elektraKs, i);
		if (keyIsDirectlyBelow (mountpointsRoot, cur) == 1)
		{
			if (!parseAndAddMountpoint (mountpoints, modules, elektraKs, cur, errorKey))
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
	addMountpoint (backends, keyName (rootKey), plugin, plugins, definition);
}

static bool addHardcodedMountpoints (KDB * handle, Key * errorKey)
{
	addElektraMountpoint (handle->backends, handle->modules, errorKey);

	Plugin * defaultResolver = elektraPluginOpen (KDB_RESOLVER, handle->modules, ksNew (0, KS_END), errorKey);
	if (defaultResolver == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open default resolver plugin. See warnings for details.");
		return false;
	}

	Plugin * defaultStorage = elektraPluginOpen (KDB_STORAGE, handle->modules, ksNew (0, KS_END), errorKey);
	if (defaultStorage == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open default storage plugin. See warnings for details.");
		elektraPluginClose (defaultResolver, errorKey);
		return false;
	}

	Plugin * root = elektraPluginOpen ("backend", handle->modules, ksNew (0, KS_END), errorKey);
	if (root == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open default backend. See warnings for details.");
		elektraPluginClose (defaultResolver, errorKey);
		elektraPluginClose (defaultStorage, errorKey);
		return false;
	}

	// clang-format off
	KeySet * rootPlugins =
		ksNew (2,
			keyNew ("/#0", KEY_BINARY, KEY_SIZE, sizeof (defaultResolver), KEY_VALUE, defaultResolver, KEY_END),
			keyNew ("/#1", KEY_BINARY, KEY_SIZE, sizeof (defaultStorage), KEY_VALUE, defaultStorage, KEY_END),
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

	Plugin * modules = elektraPluginOpen ("modules", handle->modules, ksNew (0, KS_END), errorKey);
	if (modules == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open system:/elektra/modules backend. See warnings for details.");
		elektraPluginClose (defaultResolver, errorKey);
		elektraPluginClose (defaultStorage, errorKey);
		return false;
	}
	addMountpoint (handle->backends, KDB_SYSTEM_ELEKTRA "/modules", modules, ksNew (0, KS_END), handle->modules);

	Plugin * version = elektraPluginOpen ("version", handle->modules, ksNew (0, KS_END), errorKey);
	if (version == NULL)
	{
		ELEKTRA_SET_INSTALLATION_ERROR (errorKey, "Could not open system:/elektra/version backend. See warnings for details.");
		return false;
	}
	addMountpoint (handle->backends, KDB_SYSTEM_ELEKTRA "/version", version, ksNew (0, KS_END), ksNew (0, KS_END));

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

	if (!addElektraMountpoint (handle->backends, handle->modules, errorKey))
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
	KeySet * backends = elektraMountpointsParse (elektraKs, handle->modules, errorKey);
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

/**
 * @internal
 *
 * @brief Check if an update is needed at all
 *
 * @retval -2 cache hit
 * @retval -1 an error occurred
 * @retval 0 no update needed
 * @retval number of plugins which need update
 */
static int elektraGetCheckUpdateNeeded (KeySet * backends, Key * parentKey)
{
	int updateNeededOccurred = 0;
	ssize_t cacheHits = 0;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		int ret = -1;
		Key * backendKey = ksAtCursor (backends, i);

		if (keyGetNamespace (backendKey) == KEY_NS_DEFAULT)
		{
			continue;
		}

		const BackendData * backendData = keyValue (backendKey);
		Plugin * backend = backendData->backend;

		keySetMeta (backendKey, "internal/kdb/needsync", NULL);

		if (backend && backend->kdbGet)
		{
			keyCopy (parentKey, backendKey, KEY_CP_NAME);
			keySetString (parentKey, "");
			KeySet * ks = ksNew (0, KS_END);
			ret = backend->kdbGet (backend, ks, parentKey);
			ksDel (ks);
			// store resolved filename
			keySetMeta (backendKey, "internal/kdb/filename", keyString (parentKey));
			// no keys in that backend
			ELEKTRA_LOG_DEBUG ("backend: %s,%s ;; ret: %d", keyName (parentKey), keyString (parentKey), ret);
		}

		switch (ret)
		{
		case ELEKTRA_PLUGIN_STATUS_CACHE_HIT:
			// Keys in cache are up-to-date
			++cacheHits;
			// Set sync flag, needed in case of cache miss
			// FALLTHROUGH
		case ELEKTRA_PLUGIN_STATUS_SUCCESS:
			// Seems like we need to sync that
			keySetMeta (backendKey, "internal/kdb/needsync", "1");
			++updateNeededOccurred;
			break;
		case ELEKTRA_PLUGIN_STATUS_NO_UPDATE:
			// Nothing to do here
			break;
		default:
			ELEKTRA_ASSERT (0, "resolver did not return 1 0 -1, but %d", ret);
		case ELEKTRA_PLUGIN_STATUS_ERROR:
			// Ohh, an error occurred, lets stop the
			// process.
			return -1;
		}
	}

	if (cacheHits == ksGetSize (backends))
	{
		ELEKTRA_LOG_DEBUG ("all backends report cache is up-to-date");
		return -2;
	}

	return updateNeededOccurred;
}

typedef enum
{
	FIRST,
	LAST
} UpdatePass;

/**
 * @internal
 * @brief Do the real update.
 *
 * @retval -1 on error
 * @retval 0 on success
 */
static int elektraGetDoUpdate (KeySet * backends, Key * parentKey)
{
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		const Key * backendKey = ksAtCursor (backends, i);

		if (keyGetNamespace (backendKey) == KEY_NS_DEFAULT || keyGetMeta (backendKey, "internal/kdb/needsync") == NULL)
		{
			// skip it, update is not needed
			continue;
		}

		const BackendData * backendData = keyValue (backendKey);
		Plugin * backend = backendData->backend;
		ksRewind (backendData->keys);
		keyCopy (parentKey, backendKey, KEY_CP_NAME);
		keyCopy (parentKey, keyGetMeta (backendKey, "internal/kdb/filename"), KEY_CP_STRING);

		for (size_t p = 1; p < NR_OF_GET_PLUGINS; ++p)
		{
			int ret = 0;
			if (backend && backend->kdbGet)
			{
				ret = backend->kdbGet (backend, backendData->keys, parentKey);
			}

			if (ret == -1)
			{
				// Ohh, an error occurred,
				// lets stop the process.
				return -1;
			}
		}
	}
	return 0;
}

static KeySet * prepareGlobalKS (KeySet * ks, Key * parentKey)
{
	ksRewind (ks);
	Key * cutKey = keyNew ("/", KEY_END);
	keyAddName (cutKey, strchr (keyName (parentKey), '/'));
	KeySet * cutKS = ksCut (ks, cutKey);
	Key * specCutKey = keyNew ("spec:/", KEY_END);
	KeySet * specCut = ksCut (cutKS, specCutKey);
	ksRewind (specCut);
	Key * cur;
	while ((cur = ksNext (specCut)) != NULL)
	{
		if (keyGetNamespace (cur) == KEY_NS_CASCADING)
		{
			ksAppendKey (cutKS, cur);
			keyDel (ksLookup (specCut, cur, KDB_O_POP));
		}
	}
	ksAppend (ks, specCut);
	ksDel (specCut);
	keyDel (specCutKey);
	keyDel (cutKey);
	ksRewind (cutKS);
	return cutKS;
}

static int elektraGetDoUpdateWithGlobalHooks (KDB * handle, KeySet * backends, KeySet * ks, Key * parentKey, Key * initialParent,
					      UpdatePass run)
{
	switch (run)
	{
	case FIRST:
		keySetName (parentKey, keyName (initialParent));
		elektraGlobalGet (handle, ks, parentKey, GETSTORAGE, INIT);
		elektraGlobalGet (handle, ks, parentKey, GETSTORAGE, MAXONCE);
		break;
	case LAST:
		keySetName (parentKey, keyName (initialParent));
		elektraGlobalGet (handle, ks, parentKey, PROCGETSTORAGE, INIT);
		elektraGlobalGet (handle, ks, parentKey, PROCGETSTORAGE, MAXONCE);
		elektraGlobalError (handle, ks, parentKey, PROCGETSTORAGE, DEINIT);
		break;
	default:
		break;
	}

	// elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, INIT);

	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);

		if (keyGetNamespace (backendKey) == KEY_NS_DEFAULT)
		{
			continue;
		}

		const BackendData * backendData = keyValue (backendKey);
		Plugin * backend = backendData->backend;
		ksRewind (backendData->keys);
		keyCopy (parentKey, backendKey, KEY_CP_NAME);
		keyCopy (parentKey, keyGetMeta (backendKey, "internal/kdb/filename"), KEY_CP_STRING);

		int start, end;
		if (run == FIRST)
		{
			start = 1;
			end = GET_GETSTORAGE + 1;
		}
		else
		{
			start = GET_GETSTORAGE + 1;
			end = NR_OF_GET_PLUGINS;
		}
		for (int p = start; p < end; ++p)
		{
			int ret = 0;

			if (p == GET_POSTGETSTORAGE && handle->globalPlugins[PROCGETSTORAGE][FOREACH])
			{
				keySetName (parentKey, keyName (initialParent));
				ksRewind (ks);
				handle->globalPlugins[PROCGETSTORAGE][FOREACH]->kdbGet (handle->globalPlugins[PROCGETSTORAGE][FOREACH], ks,
											parentKey);
				keyCopy (parentKey, backendKey, KEY_CP_NAME);
			}
			if (p == GET_POSTGETSTORAGE && handle->globalPlugins[POSTGETSTORAGE][FOREACH])
			{
				keySetName (parentKey, keyName (initialParent));
				ksRewind (ks);
				handle->globalPlugins[POSTGETSTORAGE][FOREACH]->kdbGet (handle->globalPlugins[POSTGETSTORAGE][FOREACH], ks,
											parentKey);
				keyCopy (parentKey, backendKey, KEY_CP_NAME);
			}
			else if (p == GET_POSTGETSTORAGE && handle->globalPlugins[POSTGETCLEANUP][FOREACH])
			{
				keySetName (parentKey, keyName (initialParent));
				ksRewind (ks);
				handle->globalPlugins[POSTGETCLEANUP][FOREACH]->kdbGet (handle->globalPlugins[POSTGETCLEANUP][FOREACH], ks,
											parentKey);
				keyCopy (parentKey, backendKey, KEY_CP_NAME);
			}

			if (backend && backend->kdbGet)
			{
				if (p <= GET_GETSTORAGE)
				{
					if (keyGetMeta (backendKey, "internal/kdb/needsync") == NULL)
					{
						// skip it, update is not needed
						continue;
					}

					ret = backend->kdbGet (backend, backendData->keys, parentKey);
				}
				else
				{
					KeySet * cutKS = prepareGlobalKS (ks, parentKey);
					ret = backend->kdbGet (backend, cutKS, parentKey);
					ksAppend (ks, cutKS);
					ksDel (cutKS);
				}
			}

			if (ret == -1)
			{
				keySetName (parentKey, keyName (initialParent));
				// Ohh, an error occurred,
				// lets stop the process.
				elektraGlobalError (handle, ks, parentKey, GETSTORAGE, DEINIT);
				// elektraGlobalError (handle, ks, parentKey, POSTGETSTORAGE, DEINIT);
				return -1;
			}
		}
	}

	if (run == FIRST)
	{
		keySetName (parentKey, keyName (initialParent));
		elektraGlobalGet (handle, ks, parentKey, GETSTORAGE, DEINIT);
		// elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, DEINIT);
	}
	return 0;
}

static int copyError (Key * dest, Key * src)
{
	keyRewindMeta (src);
	const Key * metaKey = keyGetMeta (src, "error");
	if (!metaKey) return 0;
	keySetMeta (dest, keyName (metaKey), keyString (metaKey));
	while ((metaKey = keyNextMeta (src)) != NULL)
	{
		if (strncmp (keyName (metaKey), "error/", 6)) break;
		keySetMeta (dest, keyName (metaKey), keyString (metaKey));
	}
	return 1;
}
static void clearError (Key * key)
{
	keySetMeta (key, "error", 0);
	keySetMeta (key, "error/number", 0);
	keySetMeta (key, "error/description", 0);
	keySetMeta (key, "error/reason", 0);
	keySetMeta (key, "error/module", 0);
	keySetMeta (key, "error/file", 0);
	keySetMeta (key, "error/line", 0);
	keySetMeta (key, "error/configfile", 0);
	keySetMeta (key, "error/mountpoint", 0);
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
	elektraNamespace ns = keyGetNamespace (parentKey);
	if (ns == KEY_NS_NONE)
	{
		return -1;
	}

	Key * oldError = keyNew (keyName (parentKey), KEY_END);
	copyError (oldError, parentKey);

	if (ns == KEY_NS_META)
	{
		clearError (parentKey);
		keyDel (oldError);
		ELEKTRA_SET_INTERFACE_ERRORF (parentKey, "Metakey with name '%s' passed to kdbGet as parentkey", keyName (parentKey));
		return -1;
	}

	int errnosave = errno;
	Key * initialParent = keyDup (parentKey, KEY_CP_ALL);

	ELEKTRA_LOG ("now in new kdbGet (%s)", keyName (parentKey));

#if 1 == 0
	Split * split = splitNew ();
#endif
	KeySet * backends = backendsForParentKey (handle->backends, parentKey);

	KeySet * cache = 0;
	Key * cacheParent = 0;
	int debugGlobalPositions = 0;

#ifdef DEBUG
	if (keyGetMeta (parentKey, "debugGlobalPositions") != 0)
	{
		debugGlobalPositions = 1;
	}
#endif

	if (!handle)
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "NULL pointer passed for handle");
		goto error;
	}

	if (!ks)
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "NULL pointer passed for KeySet");
		goto error;
	}

#if 1 == 0
	if (splitBuildup (split, handle, parentKey) == -1)
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERNAL_ERROR (parentKey, "Error in splitBuildup");
		goto error;
	}
#endif

// FIXME: cache
#if 2 == 0
	cache = ksNew (0, KS_END);
	cacheParent = keyDup (mountGetMountpoint (handle, initialParent), KEY_CP_NAME);
	if (ns == KEY_NS_CASCADING) keySetMeta (cacheParent, "cascading", "");
	if (handle->globalPlugins[PREGETCACHE][MAXONCE])
	{
		elektraCacheLoad (handle, cache, parentKey, initialParent, cacheParent);
	}
#endif

	// Check if a update is needed at all
	switch (elektraGetCheckUpdateNeeded (backends, parentKey))
	{
	case -2: // We have a cache hit
		goto cachemiss;
		// FIXME: cache
#if 2 == 0
		if (elektraCacheLoadSplit (handle, split, ks, &cache, &cacheParent, parentKey, initialParent, debugGlobalPositions) != 0)
		{
			goto cachemiss;
		}

		keySetName (parentKey, keyName (initialParent));
		splitUpdateFileName (split, handle, parentKey);
		keyDel (initialParent);
		splitDel (split);
		errno = errnosave;
		keyDel (oldError);
		return 1;
#endif
	case 0: // We don't need an update so let's do nothing

		if (debugGlobalPositions)
		{
			keySetName (parentKey, keyName (initialParent));
			if (elektraGlobalGet (handle, ks, parentKey, PREGETSTORAGE, INIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
			if (elektraGlobalGet (handle, ks, parentKey, PREGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
			if (elektraGlobalGet (handle, ks, parentKey, PREGETSTORAGE, DEINIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}

			keySetName (parentKey, keyName (initialParent));
			if (elektraGlobalGet (handle, ks, parentKey, PROCGETSTORAGE, INIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
			if (elektraGlobalGet (handle, ks, parentKey, PROCGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
			if (elektraGlobalGet (handle, ks, parentKey, PROCGETSTORAGE, DEINIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
		}

		ksDel (cache);
		cache = 0;
		keyDel (cacheParent);
		cacheParent = 0;

		if (debugGlobalPositions)
		{
			keySetName (parentKey, keyName (initialParent));
			if (elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, INIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
			if (elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
			if (elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, DEINIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
			{
				goto error;
			}
		}

		keySetName (parentKey, keyName (initialParent));
		keyCopy (parentKey, keyGetMeta (mountGetMountpoint (handle, parentKey), "internal/kdb/filename"), KEY_CP_STRING);
		keyDel (initialParent);
		errno = errnosave;
		keyDel (oldError);
		return 0;
	case -1:
		goto error;
		// otherwise fall trough
	}

cachemiss:
	ksDel (cache);
	cache = 0;

	if (elektraGlobalGet (handle, ks, parentKey, PREGETSTORAGE, INIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}
	if (elektraGlobalGet (handle, ks, parentKey, PREGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}
	if (elektraGlobalGet (handle, ks, parentKey, PREGETSTORAGE, DEINIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}

	// Appoint keys (some in the bypass)
	if (!backendsDivide (backends, ks))
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERNAL_ERROR (parentKey, "Error in backendsDivide");
		goto error;
	}

	if (handle->globalPlugins[POSTGETSTORAGE][FOREACH] || handle->globalPlugins[POSTGETCLEANUP][FOREACH] ||
	    handle->globalPlugins[PROCGETSTORAGE][FOREACH] || handle->globalPlugins[PROCGETSTORAGE][INIT] ||
	    handle->globalPlugins[PROCGETSTORAGE][MAXONCE] || handle->globalPlugins[PROCGETSTORAGE][DEINIT])
	{
		clearError (parentKey);
		if (elektraGetDoUpdateWithGlobalHooks (handle, backends, ks, parentKey, initialParent, FIRST) == -1)
		{
			goto error;
		}
		else
		{
			copyError (parentKey, oldError);
		}

		keySetName (parentKey, keyName (initialParent));

		// TODO: drop misplaced keys
#if 1 == 0
		if (splitGet (split, parentKey, handle) == -1)
		{
			ELEKTRA_ADD_PLUGIN_MISBEHAVIOR_WARNINGF (parentKey, "Wrong keys in postprocessing: %s", keyName (ksCurrent (ks)));
			// continue, because sizes are already updated
		}
#endif
		ksClear (ks);
		backendsMerge (backends, ks);

		clearError (parentKey);
		if (elektraGetDoUpdateWithGlobalHooks (handle, backends, ks, parentKey, initialParent, LAST) == -1)
		{
			goto error;
		}
		else
		{
			copyError (parentKey, oldError);
		}
	}
	else
	{

		/* Now do the real updating,
		   but not for bypassed keys in split->size-1 */
		clearError (parentKey);
		// do everything up to position get_storage
		if (elektraGetDoUpdate (backends, parentKey) == -1)
		{
			goto error;
		}
		else
		{
			copyError (parentKey, oldError);
		}

#if 1 == 0
		/* Now post-process the updated keysets */
		if (splitGet (split, parentKey, handle) == -1)
		{
			ELEKTRA_ADD_PLUGIN_MISBEHAVIOR_WARNINGF (parentKey, "Wrong keys in postprocessing: %s", keyName (ksCurrent (ks)));
			// continue, because sizes are already updated
		}
#endif

		ksClear (ks);
		backendsMerge (backends, ks);
	}

	keySetName (parentKey, keyName (initialParent));

	if (elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, INIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}
	if (elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, MAXONCE) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}
	if (elektraGlobalGet (handle, ks, parentKey, POSTGETSTORAGE, DEINIT) == ELEKTRA_PLUGIN_STATUS_ERROR)
	{
		goto error;
	}

// FIXME: cache
#if 2 == 0
	if (handle->globalPlugins[POSTGETCACHE][MAXONCE])
	{
		splitCacheStoreState (handle, split, handle->global, cacheParent, initialParent);
		KeySet * proc = elektraCutProc (ks); // remove proc keys before caching
		if (elektraGlobalSet (handle, ks, cacheParent, POSTGETCACHE, MAXONCE) != ELEKTRA_PLUGIN_STATUS_SUCCESS)
		{
			ELEKTRA_LOG_DEBUG ("CACHE ERROR: could not store cache");
			// we must remove the stored split state from the global keyset
			// if there was an error, otherwise we get erroneous cache hits
			elektraCacheCutMeta (handle);
		}
		elektraRestoreProc (ks, proc);
	}
	else
	{
		elektraCacheCutMeta (handle);
	}
#endif
	keyDel (cacheParent);
	cacheParent = 0;

	// the default split is not handled by POSTGETSTORAGE
	Key * defaultBackendKey = ksLookupByName (backends, "default:/", 0);
	if (defaultBackendKey != NULL)
	{
		const BackendData * defaultBackendData = keyValue (defaultBackendKey);
		ksAppend (ks, defaultBackendData->keys);
	}

	ksRewind (ks);

	keySetName (parentKey, keyName (initialParent));

	keyCopy (parentKey, keyGetMeta (mountGetMountpoint (handle, parentKey), "internal/kdb/filename"), KEY_CP_STRING);
	keyDel (initialParent);
	keyDel (oldError);
	errno = errnosave;
	return 1;

error:
	ELEKTRA_LOG_DEBUG ("now in error state");
	if (cacheParent) keyDel (cacheParent);
	if (cache) ksDel (cache);
	keySetName (parentKey, keyName (initialParent));
	elektraGlobalError (handle, ks, parentKey, POSTGETSTORAGE, INIT);
	elektraGlobalError (handle, ks, parentKey, POSTGETSTORAGE, MAXONCE);
	elektraGlobalError (handle, ks, parentKey, POSTGETSTORAGE, DEINIT);

	keySetName (parentKey, keyName (initialParent));
	if (handle)
	{
		keyCopy (parentKey, keyGetMeta (mountGetMountpoint (handle, parentKey), "internal/kdb/filename"), KEY_CP_STRING);
	}
	keyDel (initialParent);
	keyDel (oldError);
	errno = errnosave;
	return -1;
}

/**
 * @internal
 * @brief Does all set steps but not commit
 *
 * @param split all information for iteration
 * @param parentKey to add warnings (also passed to plugins for the same reason)
 * @param [out] errorKey may point to which key caused the error or 0 otherwise
 *
 * @retval -1 on error
 * @retval 0 on success
 */
static int elektraSetPrepare (KeySet * backends, Key * parentKey, Key ** errorKey, Plugin * hooks[][NR_GLOBAL_SUBPOSITIONS])
{
	int any_error = 0;
	for (elektraCursor i = 0; i < ksGetSize (backends); i++)
	{
		Key * backendKey = ksAtCursor (backends, i);

		if (keyGetNamespace (backendKey) == KEY_NS_DEFAULT || strcmp (keyName (backendKey), "system:/elektra/version") == 0)
		{
			continue;
		}

		const BackendData * backendData = keyValue (backendKey);
		for (size_t p = 0; p < SET_COMMIT; ++p)
		{
			int ret = 0; // last return value

			Plugin * backend = backendData->backend;
			ksRewind (backendData->keys);
			if (backend && backend->kdbSet)
			{
				if (p != 0)
				{
					keyCopy (parentKey, keyGetMeta (backendKey, "internal/kdb/filename"), KEY_CP_STRING);
				}
				else
				{
					keySetString (parentKey, "");
				}
				keyCopy (parentKey, backendKey, KEY_CP_NAME);
				ret = backend->kdbSet (backend, backendData->keys, parentKey);

#if VERBOSE && DEBUG
				printf ("Prepare %s with keys %zd in plugin: %zu, split: %zu, ret: %d\n", keyName (parentKey),
					ksGetSize (backendData->keys), p, i, ret);
#endif

				if (p == 0)
				{
					if (ret == 0)
					{
						// resolver says that sync is
						// not needed, so we
						// skip other pre-commit
						// plugins
						break;
					}
					keySetMeta (backendKey, "internal/kdb/filename", keyString (parentKey));
				}
			}

			if (p == 0)
			{
				if (hooks[PRESETSTORAGE][FOREACH])
				{
					ksRewind (backendData->keys);
					hooks[PRESETSTORAGE][FOREACH]->kdbSet (hooks[PRESETSTORAGE][FOREACH], backendData->keys, parentKey);
				}
			}
			else if (p == (SET_SETSTORAGE - 1))
			{
				if (hooks[PRESETCLEANUP][FOREACH])
				{
					ksRewind (backendData->keys);
					hooks[PRESETCLEANUP][FOREACH]->kdbSet (hooks[PRESETCLEANUP][FOREACH], backendData->keys, parentKey);
				}
			}

			if (ret == -1)
			{
				// do not
				// abort because it might
				// corrupt the KeySet
				// and leads to warnings
				// because of .tmp files not
				// found
				*errorKey = ksCurrent (backendData->keys);

				// so better keep going, but of
				// course we will not commit
				any_error = -1;
			}
		}
	}
	return any_error;
}

/**
 * @internal
 * @brief Does the commit
 *
 * @param split all information for iteration
 * @param parentKey to add warnings (also passed to plugins for the same reason)
 */
static void elektraSetCommit (KeySet * backends, Key * parentKey)
{
	for (size_t p = SET_COMMIT; p < NR_OF_SET_PLUGINS; ++p)
	{
		for (elektraCursor i = 0; i < ksGetSize (backends); i++)
		{
			Key * backendKey = ksAtCursor (backends, i);

			if (keyGetNamespace (backendKey) == KEY_NS_DEFAULT)
			{
				continue;
			}

			const BackendData * backendData = keyValue (backendKey);
			Plugin * backend = backendData->backend;

			if (backend && backend->kdbSet)
			{
				if (p != SET_COMMIT)
				{
					keySetString (parentKey, keyString (keyGetMeta (backendKey, "internal/kdb/filename")));
				}
				keyCopy (parentKey, backendKey, KEY_CP_NAME);
#if DEBUG && VERBOSE
				printf ("elektraSetCommit: %p # %zu with %s - %s\n", backend, p, keyName (parentKey),
					keyString (parentKey));
#endif
				ksRewind (backendData->keys);

				int ret = 0;
				if (p == SET_COMMIT)
				{
					ret = backend->kdbCommit (backend, backendData->keys, parentKey);
					// name of non-temp file
					keySetMeta (backendKey, "internal/kdb/filename", keyString (parentKey));
				}
				else
				{
					ret = backend->kdbSet (backend, backendData->keys, parentKey);
				}

				if (ret == -1)
				{
					ELEKTRA_ADD_INTERNAL_WARNINGF (parentKey, "Error during commit. This means backend is broken: %s",
								       keyName (backendKey));
				}
			}
		}
	}
}

/**
 * @internal
 * @brief Does the rollback
 *
 * @param split all information for iteration
 * @param parentKey to add warnings (also passed to plugins for the same reason)
 */
static void elektraSetRollback (KeySet * backends, Key * parentKey)
{
	for (size_t p = 0; p < NR_OF_ERROR_PLUGINS; ++p)
	{
		for (elektraCursor i = 0; i < ksGetSize (backends); i++)
		{
			int ret = 0;
			const Key * backendKey = ksAtCursor (backends, i);

			if (keyGetNamespace (backendKey) == KEY_NS_DEFAULT)
			{
				continue;
			}

			const BackendData * backendData = keyValue (backendKey);
			Plugin * backend = backendData->backend;

			ksRewind (backendData->keys);
			if (backend && backend->kdbError)
			{
				keyCopy (parentKey, backendKey, KEY_CP_NAME);
				ret = backend->kdbError (backend, backendData->keys, parentKey);
			}

			if (ret == -1)
			{
				ELEKTRA_ADD_INTERNAL_WARNINGF (parentKey, "Error during rollback. This means backend is broken: %s",
							       keyName (backendGetMountpoint (backend)));
			}
		}
	}
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

	if (test_bit (parentKey->flags, KEY_FLAG_RO_NAME))
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "parentKey with read-only name passed");
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_NAME");
		return -1;
	}

	if (test_bit (parentKey->flags, KEY_FLAG_RO_VALUE))
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "parentKey with read-only value passed");
		ELEKTRA_LOG ("parentKey KEY_FLAG_RO_VALUE");
		return -1;
	}

	if (handle == NULL)
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "KDB handle null pointer passed");
		ELEKTRA_LOG ("handle == NULL");
		return -1;
	}

	if (ks == NULL)
	{
		clearError (parentKey);
		ELEKTRA_SET_INTERFACE_ERROR (parentKey, "KeySet null pointer passed");
		ELEKTRA_LOG ("ks == NULL");
		return -1;
	}

	elektraNamespace ns = keyGetNamespace (parentKey);
	Key * oldError = keyNew (keyName (parentKey), KEY_END);
	copyError (oldError, parentKey);

	if (ns == KEY_NS_META)
	{
		clearError (parentKey); // clear previous error to set new one
		ELEKTRA_SET_INTERFACE_ERRORF (parentKey, "Metakey with name '%s' passed to kdbSet as parentkey", keyName (parentKey));
		keyDel (oldError);
		ELEKTRA_LOG ("ns == KEY_NS_META");
		return -1;
	}

	int errnosave = errno;
	Key * initialParent = keyDup (parentKey, KEY_CP_ALL);

	ELEKTRA_LOG ("now in new kdbSet (%s) %p %zd", keyName (parentKey), (void *) handle, ksGetSize (ks));

	elektraGlobalSet (handle, ks, parentKey, PRESETSTORAGE, INIT);
	elektraGlobalSet (handle, ks, parentKey, PRESETSTORAGE, MAXONCE);
	elektraGlobalSet (handle, ks, parentKey, PRESETSTORAGE, DEINIT);

	ELEKTRA_LOG ("after presetstorage maxonce(%s) %p %zd", keyName (parentKey), (void *) handle, ksGetSize (ks));

#if 1 == 0
	Split * split = splitNew ();
	Key * errorKey = 0;

	if (splitBuildup (split, handle, parentKey) == -1)
	{
		clearError (parentKey); // clear previous error to set new one
		ELEKTRA_SET_INTERNAL_ERROR (parentKey, "Error in splitBuildup");
		goto error;
	}
	ELEKTRA_LOG ("after splitBuildup");

	// 1.) Search for syncbits
	int syncstate = splitDivide (split, handle, ks);
	if (syncstate == -1)
	{
		clearError (parentKey); // clear previous error to set new one
		ELEKTRA_SET_INSTALLATION_ERRORF (parentKey, "No default backend found, but should be. Keyname: %s",
						 keyName (ksCurrent (ks)));
		goto error;
	}
	ELEKTRA_ASSERT (syncstate == 0 || syncstate == 1, "syncstate not 0 or 1, but %d", syncstate);
	ELEKTRA_LOG ("after 1.) Search for syncbits");

	// 2.) Search for changed sizes
	syncstate |= splitSync (split);
	ELEKTRA_ASSERT (syncstate <= 1, "syncstate not equal or below 1, but %d", syncstate);
	if (syncstate != 1)
	{
		/* No update is needed */
		ELEKTRA_LOG ("No update is needed");
		keySetName (parentKey, keyName (initialParent));
		if (syncstate < 0) clearError (parentKey); // clear previous error to set new one
		if (syncstate == -1)
		{
			ELEKTRA_SET_INTERNAL_ERROR (parentKey, "Assert failed: invalid namespace");
			ELEKTRA_LOG ("syncstate == -1");
		}
		else if (syncstate < -1)
		{
			ELEKTRA_SET_CONFLICTING_STATE_ERRORF (
				parentKey, "Sync state is wrong, maybe 'kdbSet()' is executed without prior 'kdbGet()' on %s",
				keyName (split->parents[-syncstate - 2]));
			ELEKTRA_LOG ("syncstate < -1");
		}
		keyDel (initialParent);
		splitDel (split);
		errno = errnosave;
		keyDel (oldError);
		ELEKTRA_LOG ("return: %d", syncstate == 0 ? 0 : -1);
		return syncstate == 0 ? 0 : -1;
	}
	ELEKTRA_ASSERT (syncstate == 1, "syncstate not 1, but %d", syncstate);
	ELEKTRA_LOG ("after 2.) Search for changed sizes");

	splitPrepare (split);
#endif
	// FIXME: ensure kdbGet() called first

	KeySet * backends = backendsForParentKey (handle->backends, parentKey);

	if (!backendsDivide (backends, ks))
	{
		/* Error during backend divison */
		ELEKTRA_LOG ("Error during backend divison");
		keySetName (parentKey, keyName (initialParent));
		clearError (parentKey); // clear previous error to set new one

		ELEKTRA_SET_INTERNAL_ERROR (parentKey, "Assert failed: invalid namespace");

		keyDel (initialParent);
		errno = errnosave;
		keyDel (oldError);
		ELEKTRA_LOG ("return: -1");
		return -1;
	}

	// TODO: complain about misplaced keys (divided into default:/)

	clearError (parentKey); // clear previous error to set new one

	Key * errorKey = 0;
	if (elektraSetPrepare (backends, parentKey, &errorKey, handle->globalPlugins) == -1)
	{
		goto error;
	}
	else
	{
		// no error, restore old error
		copyError (parentKey, oldError);
	}
	keySetName (parentKey, keyName (initialParent));

	elektraGlobalSet (handle, ks, parentKey, PRECOMMIT, INIT);
	elektraGlobalSet (handle, ks, parentKey, PRECOMMIT, MAXONCE);
	elektraGlobalSet (handle, ks, parentKey, PRECOMMIT, DEINIT);

	elektraSetCommit (backends, parentKey);

	elektraGlobalSet (handle, ks, parentKey, COMMIT, INIT);
	elektraGlobalSet (handle, ks, parentKey, COMMIT, MAXONCE);
	elektraGlobalSet (handle, ks, parentKey, COMMIT, DEINIT);

	keySetName (parentKey, keyName (initialParent));

	elektraGlobalSet (handle, ks, parentKey, POSTCOMMIT, INIT);
	elektraGlobalSet (handle, ks, parentKey, POSTCOMMIT, MAXONCE);
	elektraGlobalSet (handle, ks, parentKey, POSTCOMMIT, DEINIT);

	for (size_t i = 0; i < ks->size; ++i)
	{
		// remove all flags from all keys
		clear_bit (ks->array[i]->flags, (keyflag_t) KEY_FLAG_SYNC);
	}

	keySetName (parentKey, keyName (initialParent));
	keyDel (initialParent);

	keyDel (oldError);
	errno = errnosave;
	ELEKTRA_LOG ("before RETURN 1");
	return 1;

error:
	keySetName (parentKey, keyName (initialParent));

	elektraGlobalError (handle, ks, parentKey, PREROLLBACK, INIT);
	elektraGlobalError (handle, ks, parentKey, PREROLLBACK, MAXONCE);
	elektraGlobalError (handle, ks, parentKey, PREROLLBACK, DEINIT);

	elektraSetRollback (backends, parentKey);

	if (errorKey)
	{
		Key * found = ksLookup (ks, errorKey, 0);
		if (!found)
		{
			ELEKTRA_ADD_INTERNAL_WARNINGF (parentKey, "Error key %s not found in keyset even though it was found before",
						       keyName (errorKey));
		}
	}

	keySetName (parentKey, keyName (initialParent));

	elektraGlobalError (handle, ks, parentKey, POSTROLLBACK, INIT);
	elektraGlobalError (handle, ks, parentKey, POSTROLLBACK, MAXONCE);
	elektraGlobalError (handle, ks, parentKey, POSTROLLBACK, DEINIT);

	keySetName (parentKey, keyName (initialParent));
	keyDel (initialParent);
	errno = errnosave;
	keyDel (oldError);
	return -1;
}

/**
 * @}
 */
