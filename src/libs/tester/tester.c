#ifdef ELEKTRA_STATIC
#define TESTFUNCTION_NAME libelektra_tester_LTX_testfunction
#else
#define TESTFUNCTION_NAME testfunction
#endif

int TESTFUNCTION_NAME (int a, int b)
{
	return a * b + a / b;
}
