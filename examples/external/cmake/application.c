/**
 * @file
 *
 * @brief
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 */

#include <kdb.h>

#include <stdio.h>

#include <kdbmodule.h>

typedef int (*testfunction_t) (int, int);

int main (void)
{
	KeySet * myConfig = ksNew (0, KS_END);
	Key * key = keyNew ("system:/test/myapp", KEY_END);
	KDB * handle = kdbOpen (key);

	kdbGet (handle, myConfig, key);

	keySetName (key, "user:/test/myapp");
	kdbGet (handle, myConfig, key);

	// check for errors in key
	keyDel (key);

	key = ksLookupByName (myConfig, "/test/myapp/key", 0);

	// check if key is not 0 and work with it...
	if (key)
	{
		printf ("%s\n", keyString (key));
	}

	ksDel (myConfig); // delete the in-memory configuration


	// maybe you want kdbSet() myConfig here

	kdbClose (handle, 0); // no more affairs with the key database.

	KeySet * modules = ksNew (0, KS_END);
	Key * errorKey = keyNew ("/", KEY_END);
	testfunction_t testfunction = (testfunction_t) elektraModulesLoad (modules, "tester", "testfunction", errorKey);
	printf ("TEST: %d\n", testfunction (42, 19));

	return 0;
}
