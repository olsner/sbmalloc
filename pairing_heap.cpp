
/**
 * These are adapted to be stored internally in whatever data we have.
 */
struct pairing_ptr_node
{
	/**
	 * down,right: first child and right sibling of this page in pairing heap
	 * of pages with free space.
	 */
	pairing_ptr_node* down;
	pairing_ptr_node* right;
};
struct pairing_ptr_heap
{
	typedef pairing_ptr_node T;
	typedef pairing_ptr_node* Tp;

	Tp min;

	void delete_min()
	{
		assert(!min->right);
		Tp l = min->down;
		min->down = NULL;
		min = mergePairs(l);
	}

	void insert(Tp r)
	{
		min = merge(min, r);
	}

	operator bool() const
	{
		return (bool)min;
	}

	static Tp mergePairs(Tp l)
	{
		if (!l || !l->right) return l;

		Tp r = l->right;
		Tp hs = r->right;
		l->right = r->right = NULL;
		assert(hs != l && hs != r && r != l);
		// FIXME recursion...
		// We can use l->right after merge returns, since merge() always
		// returns something with a right-value of NULL. l will never be
		// touched until we're about to merge it with the result of mergePairs
		l = merge(l,r);
		hs = mergePairs(hs);
		return merge(l,hs);
	}

	static Tp merge(Tp l, Tp r)
	{
		if (!l)
		{
			assert(!r->right);
			return r;
		}
		if (!r)
		{
			assert(!l->right);
			return l;
		}

		assert(!l->right && !r->right);

		if (r < l)
		{
			Tp tmp = r;
			r = l;
			l = tmp;
		}
		// l <= r!

		r->right = l->down;
		l->down = r;
		//l->right = NULL; // we know it's already null
		return l;
	}
};
static pairing_ptr_node* get_min(const pairing_ptr_heap& p)
{
	return p.min;
}
static void delete_min(pairing_ptr_heap& p)
{
	assert(p.min);
	p.delete_min();
}
static void insert(pairing_ptr_heap& heap, pairing_ptr_node* r)
{
	heap.insert(r);
}
