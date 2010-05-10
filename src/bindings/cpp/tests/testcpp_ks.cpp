#include <tests.h>

#include <vector>
#include <algorithm>

void test_ksnew()
{
	cout << "testing keyset new" << endl;

	KeySet ks1;

	KeySet ks2 (5,
		ckdb::keyNew ("user/key2", KEY_END),
		KS_END);

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_REMOVE, KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);
	// ks3.toStream(stdout, 0);

	Key k1("user/key4/1", KEY_END);
	Key k2("user/key4/2", KEY_REMOVE, KEY_END);
	Key k3("user/key4/3", KEY_VALUE, "value", KEY_END);
	KeySet ks4 (5,
		*k1, // k1 will lose its key and pass it to keyset
		*k2,
		*k3,
		KS_END);
	// ks4.toStream(stdout, 0);

	Key k4("user/key5/1", KEY_END);
	Key k5("user/key5/2", KEY_REMOVE, KEY_END);
	Key k6("user/key5/3", KEY_VALUE, "value", KEY_END);
	KeySet ks5 (5,
		k4.dup(),
		k5.dup(),
		k6.dup(),
		KS_END);
	// ks5.toStream(stdout, 0);
	// k4, k5, k6 can still be used
}

void test_ksdup()
{
	cout << "testing ksdup" << endl;

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_REMOVE, KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	KeySet ks4 (ks3.dup());
	succeed_if (ks3.size() == 3, "size not correct");
	succeed_if (ks4.size() == 3, "size not correct");

	// ks3.toStream(stdout, 0);
	// ks4.toStream(stdout, 0);
}

void test_kscopy()
{
	cout << "testing ksdup" << endl;

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_REMOVE, KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	KeySet ks4 (ks3);
	succeed_if (ks3.size() == 3, "size not correct");
	succeed_if (ks4.size() == 3, "size not correct");

	KeySet ks5;
	ks5.copy(ks4);
	succeed_if (ks4.size() == 3, "size not correct");
	succeed_if (ks5.size() == 3, "size not correct");

	ks5.clear();
	succeed_if (ks5.size() == 0, "size not correct");



	// ks3.toStream(stdout, 0);
	// ks4.toStream(stdout, 0);
}

void test_iterate()
{
	cout << "testing iterate" << endl;

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	ks3.rewind();

	Key k1 = ks3.next();
	succeed_if (k1.getName() == "user/key3/1", "wrong keyname");
	succeed_if (k1 == ks3.head(), "first key not head key");
	Key k2 = ks3.next();
	succeed_if (k2.getName() == "user/key3/2", "wrong keyname");
	Key k3 = ks3.next();
	succeed_if (k3.getName() == "user/key3/3", "wrong keyname");
	succeed_if (k3.getString() == "value", "wrong value");
	succeed_if (k3 == ks3.tail(), "last key not tail key");
	succeed_if (!ks3.next(), "no more key");
	succeed_if (!ks3.next(), "no more key");
	succeed_if (!ks3.next(), "no more key");
	succeed_if (!ks3.next(), "no more key");

	Key null = static_cast<ckdb::Key*>(0);
	succeed_if (!null, "null key");

	ks3.rewind();
	for (size_t i=0; i<ks3.size(); i++)
	{
		Key k = ks3.next();
		char str[] = "user/key3/X";

		str [10] = i+'1';
		succeed_if (k.getName() == str, "wrong keyname");
	}

	ks3.rewind();
	Key n;
	int j=0;
	while (n=ks3.next())
	{
		char str[] = "user/key3/X";

		str [10] = j+'1';
		succeed_if (n.getName() == str, "wrong keyname");
		j++;
	}

	j=0;
	ks3.rewind();
	while ((n=ks3.next()) == true)
	{
		char str[] = "user/key3/X";

		str [10] = j+'1';
		succeed_if (n.getName() == str, "wrong keyname");
		j++;
	}

	j=0;
	ks3.rewind();
	for (Key k; k=ks3.next();)
	{
		char str[] = "user/key3/X";

		str [10] = j+'1';
		succeed_if (k.getName() == str, "wrong keyname");
		j++;
	}

	j=0;
	ks3.rewind();
	for (Key k=ks3.next(); k; k=ks3.next())
	{
		char str[] = "user/key3/X";

		str [10] = j+'1';
		succeed_if (k.getName() == str, "wrong keyname");
		j++;
	}
}

void test_cursor()
{
	cout << "testing cursor" << endl;

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);
	cursor_t cursorTest;

	ks3.rewind();
	for (size_t i=0; i<ks3.size(); i++)
	{
		Key k = ks3.next();
		if (i==0) cursorTest = ks3.getCursor();
	}

	ks3.setCursor (cursorTest);
	Key k1 = ks3.current();
	succeed_if (k1.getName() == "user/key3/1", "wrong keyname");
	succeed_if (k1 == ks3.head(), "first key not head key");
}

void test_pop()
{
	cout << "testing iterate" << endl;

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	ks3.rewind();

	Key k3 = ks3.pop();
	succeed_if (k3.getName() == "user/key3/3", "wrong keyname");
	succeed_if (k3.getString() == "value", "wrong value");
	Key k2 = ks3.pop();
	succeed_if (k2.getName() == "user/key3/2", "wrong keyname");
	Key k1 = ks3.pop();
	succeed_if (k1.getName() == "user/key3/1", "wrong keyname");
	try {
		ks3.pop();
		succeed_if (false, "Out of Range not catched");
	} catch (KeySetOutOfRange) { }

	KeySet ks4 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	ks4.rewind();
	for (int i=ks4.size()-1; i>0; i--)
	{
		Key k = ks4.pop();
		char str[] = "user/key3/X";

		str [10] = i+'1';
		succeed_if (k.getName() == str, str);
	}
}


void test_lookup()
{
	cout << "testing lookup" << endl;

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_REMOVE, KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	Key k1 = ks3.lookup("user/key3/1");
	succeed_if (k1.getName() == "user/key3/1", "wrong keyname");

	Key k3 = ks3.lookup("user/key3/3");
	succeed_if (k3.getName() == "user/key3/3", "wrong keyname");
	succeed_if (k3.getString() == "value", "wrong value");

	try {
		ks3.lookup("user/key3/2");
		succeed_if (false, "Not Found not thrown for removed key");
	} catch (KeySetNotFound) { }

	try {
		ks3.lookup("user/key3/4");
		succeed_if (false, "Not Found not thrown for not existing key");
	} catch (KeySetNotFound) { }
}


void test_append()
{
	cout << "testing keyset append" << endl;

	KeySet ks1;

	KeySet ks2 (5,
		ckdb::keyNew ("user/key2", KEY_END),
		KS_END);
	ks1.append (ks2);

	KeySet ks3 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_REMOVE, KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);
	ks2.append (ks3);
	ks1.append (ks3);
	ks3.append (ks2);

	Key k1("user/key4/1", KEY_END);
	Key k2("user/key4/2", KEY_REMOVE, KEY_END);
	Key k3("user/key4/3", KEY_VALUE, "value", KEY_END);
	ks1.append (k1); ks1.append (k2); ks1.append (k3);
	ks2.append (k1); ks2.append (k2); ks2.append (k3);
	ks3.append (k1); ks3.append (k2); ks3.append (k3);

	KeySet ks4 (5,
		*Key ("user/key3/1", KEY_END),
		*Key ("user/key3/2", KEY_REMOVE, KEY_END),
		*Key ("user/key3/3", KEY_VALUE, "value", KEY_END),
		KS_END);

	KeySet ks5;
	std::vector<Key> v(3);
	ks5.append(v[1]=Key("user/s/2", KEY_END));
	ks5.append(v[0]=Key("user/s/1", KEY_END));
	ks5.append(v[2]=Key("user/s/3", KEY_END));

	ks5.rewind();
	for (size_t i=0; i<ks5.size(); ++i)
	{
		succeed_if (ks5.next().name() == v[i].name(), "wrong order");
	}

	// ks1.toStream();
	// ks2.toStream();
	// ks3.toStream();
}

void test_per()
{
	cout << "testing keyset append with all permutations" << endl;

	vector <Key> solution;
	solution.push_back(Key("user/s/1", KEY_END));
	solution.push_back(Key("user/s/2", KEY_END));
	solution.push_back(Key("user/s/3", KEY_END));

	vector <Key> permutation(solution);

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

	solution.push_back(Key("user/s/x", KEY_END));
	permutation.push_back(solution[3]); // need a copy of same key, otherwise name is not the same string
	sort(permutation.begin(), permutation.end());

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.append(permutation[3]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			// note: char*==char* checks the identity! It needs to be the same reference
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

	solution.push_back(Key("user/x/y", KEY_END));
	permutation.push_back(solution[4]);
	sort(permutation.begin(), permutation.end());

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.append(permutation[3]);
		ks.append(permutation[4]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			// note: char*==char* checks the identity! It needs to be the same reference
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

	solution.push_back(Key("user/x/y/z", KEY_END));
	permutation.push_back(solution[5]);
	sort(permutation.begin(), permutation.end());

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.append(permutation[3]);
		ks.append(permutation[4]);
		ks.append(permutation[5]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			// note: char*==char* checks the identity! It needs to be the same reference
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

}

void test_appendowner()
{
	cout << "testing appending with owner" << endl;

	KeySet ks;
	std::vector<Key> v(3);
	ks.append(v[1]=Key("user/s/1", KEY_OWNER, "markus", KEY_END));
	ks.append(v[0]=Key("user/s/1", KEY_END));
	ks.append(v[2]=Key("user/s/1", KEY_OWNER, "max", KEY_END));

	ks.rewind();
	for (size_t i=0; i<ks.size(); ++i)
	{
		succeed_if (ks.next().name() == v[i].name(), "wrong order");
	}
}

void test_perowner()
{
	cout << "testing keyset append with owner with all permutations" << endl;

	vector <Key> solution;
	solution.push_back(Key("user/s", KEY_END));
	solution.push_back(Key("user/s", KEY_OWNER, "albert", KEY_END));
	solution.push_back(Key("user/s", KEY_OWNER, "barbara", KEY_END));

	vector <Key> permutation(solution);

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

	solution.push_back(Key("user/s", KEY_OWNER, "markus", KEY_END));
	permutation.push_back(solution[3]); // need a copy of same key, otherwise name is not the same string
	sort(permutation.begin(), permutation.end());

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.append(permutation[3]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			// note: char*==char* checks the identity! It needs to be the same reference
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

	solution.push_back(Key("user/s", KEY_OWNER, "max", KEY_END));
	permutation.push_back(solution[4]);
	sort(permutation.begin(), permutation.end());

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.append(permutation[3]);
		ks.append(permutation[4]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			// note: char*==char* checks the identity! It needs to be the same reference
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

	solution.push_back(Key("user/s", KEY_OWNER, "patrick", KEY_END));
	permutation.push_back(solution[5]);
	sort(permutation.begin(), permutation.end());

	do {
		KeySet ks;
		ks.append(permutation[0]);
		ks.append(permutation[1]);
		ks.append(permutation[2]);
		ks.append(permutation[3]);
		ks.append(permutation[4]);
		ks.append(permutation[5]);
		ks.rewind();
		for (size_t i=0; i<ks.size(); ++i)
		{
			// note: char*==char* checks the identity! It needs to be the same reference
			succeed_if (ks.next().name() == solution[i].name(), "wrong order");
		}
	} while (next_permutation(permutation.begin(), permutation.end()));

}

void test_cmp()
{
	cout << "testing comparison of keys" << endl;

	Key ke1, ke2;

	succeed_if (ke1 == ke2, "two empty keys are not the same?")
	succeed_if (!(ke1 != ke2), "two empty keys are not the same?")

	Key k1("user/a", KEY_END), k2("user/b", KEY_END);

	succeed_if (ke1 < k1, "compare empty key with user/a")
	succeed_if (ke1 <= k1, "compare empty key with user/a")
	succeed_if (!(ke1 > k1), "compare empty key with user/a")
	succeed_if (!(ke1 >= k1), "compare empty key with user/a")

	succeed_if (ke1 < k2, "compare empty key with user/b")
	succeed_if (ke1 <= k2, "compare empty key with user/b")
	succeed_if (!(ke1 > k2), "compare empty key with user/b")
	succeed_if (!(ke1 >= k2), "compare empty key with user/b")

	succeed_if (k1 < k2, "compare key user/a with user/b")
	succeed_if (k1 <= k2, "compare key user/a with user/b")
	succeed_if (!(k1 > k2), "compare key user/a with user/b")
	succeed_if (!(k1 >= k2), "compare key user/a with user/b")
	succeed_if (k1 != k2, "compare key user/a with user/b")
	succeed_if (!(k1 == k2), "compare key user/a with user/b")

	Key ko1("user/a", KEY_OWNER, "markus", KEY_END), ko2("user/b", KEY_OWNER, "max", KEY_END);

	succeed_if (ko1 > k1, "compare key with user/a")
	succeed_if (ko1 >= k1, "compare key with user/a")
	succeed_if (!(ko1 < k1), "compare key with user/a")
	succeed_if (!(ko1 <= k1), "compare key with user/a")

	succeed_if (ko2 > k2, "compare key with user/b")
	succeed_if (ko2 >= k2, "compare key with user/b")
	succeed_if (!(ko2 < k2), "compare key with user/b")
	succeed_if (!(ko2 <= k2), "compare key with user/b")

	Key ko ("user/a", KEY_OWNER, "max", KEY_END);

	succeed_if (ko1 < ko, "compare key with user/b")
	succeed_if (ko1 <= ko, "compare key with user/b")
	succeed_if (!(ko1 > ko), "compare key with user/b")
	succeed_if (!(ko1 >= ko), "compare key with user/b")

	succeed_if (ko1 < ko2, "compare key user/a with     user/a owner max")
	succeed_if (ko1 <= ko2, "compare key user/a with    user/a owner max")
	succeed_if (!(ko1 > ko2), "compare key user/a with  user/a owner max")
	succeed_if (!(ko1 >= ko2), "compare key user/a with user/a owner max")
	succeed_if (ko1 != ko2, "compare key user/a with    user/a owner max")
	succeed_if (!(ko1 == ko2), "compare key user/a with user/a owner max")
}


int main()
{
	cout << "KEYSET CLASS TESTS" << endl;
	cout << "==================" << endl << endl;

	test_ksnew();
	test_ksdup();
	test_kscopy();
	test_iterate();
	test_cursor();
	test_pop();
	test_lookup();
	test_append();
	test_per();
	test_appendowner();
	test_perowner();
	test_cmp();

	cout << endl;
	cout << "test_key RESULTS: " << nbTest << " test(s) done. " << nbError << " error(s)." << endl;
	return nbError;
}
