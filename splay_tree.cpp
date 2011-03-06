
struct splay_node
{
	typedef splay_node S;
	typedef S* Sp;

	Sp left;
	Sp right;

};
struct splay_tree
{
	typedef splay_node S;
	typedef S* Sp;

	Sp root;
	Sp min;

	splay_tree():
		root(NULL),
		min(NULL)
	{}

	operator bool()
	{
		return !!root;
	}

	size_t slow_count() const
	{
		return slow_count(root);
	}

	static size_t slow_count(Sp root)
	{
		return root ? 1 + slow_count(root->left) + slow_count(root->right) : 0;
	}

	void remove(Sp node)
	{
		assert(root);
		IFDEBUG(size_t old_count = slow_count();)
		if (node == min)
		{
			delete_min();
		}
		else
		{
			root = remove(root, node);
		}
		assert(slow_count() == old_count - 1);
	}

	static Sp remove(Sp root, Sp node)
	{
		Sp small, big;
		partition(node, root, &small, &big);
#ifdef DEBUG
		debug("Remove %p. Smaller:\n", node);
		dump_inorder(small, 0);
		debug("Remove %p. Bigger:\n", node);
		dump_inorder(big, 0);
		debug("small %p, node %p, small->left %p, big %p\n", small, node, small->left, big);
#endif
		// It's an error to remove something not in the tree
		assert(small);
		// After the partition, node must be the max of the left tree
		assert(!node->right);

		Sp dummy;
		partition(node-1, small, &small, &dummy);
		// dummy must only contain node, and node must not have any children
		assert(dummy == node && !node->left && !node->right);

		// Insert the big tree in place of 'node' in the small tree
		Sp p = small;
		while (p->right && p->right != node) p = p->right;
		p->right = big;

#ifdef DEBUG
		debug("Remove %p. Done:\n", node);
		dump_inorder(small, 0);
		debug("End.\n");
#endif

		return small;
	}

	void remove_to_end(Sp start)
	{
		root = remove_to_end(root, start);
		if (min >= start)
		{
			assert(!root);
			min = NULL;
		}
	}

	static Sp remove_to_end(Sp root, Sp start)
	{
		Sp dummy;
		partition(start-1, root, &root, &dummy);
		assert(find_min(dummy) <= start);
		return root;
	}

	void delete_min()
	{
		assert(root);
		min = delete_min(root, &root, NULL);
		assert(min || !root);
		assert(min == find_min(root));
	}

	static Sp delete_min(Sp node, Sp* ret, Sp min)
	{
		// node is the minimum node, replace with its right-subtree
		if (!node->left)
		{
			Sp right = node->right;
			*ret = right;
			return right ? find_min(right) : min;
		}
		// node->left is the minimum node, replace it with its right-subtree
		else if (!node->left->left)
		{
			Sp left = node->left->right;
			node->left = left;
			*ret = node;
			return find_min(node);
		}
		else
		{
			Sp x = node->left, a = x->left, b = x->right;
			Sp y = node; //Sp c = y->right;
			x->right = y;
			y->left = b;
			// c == y->right
			//y->right = c;
			*ret = x;
			return delete_min(a, &x->left, x);
		}
	}

	void insert(Sp node)
	{
		Sp small, big;
		partition(node, root, &small, &big);
		// node already in tree, just stitch it back together
		if (small == node)
		{
			assert(!small->right);
			node->right = big;
		}
		else
		{
			node->left = small;
			node->right = big;
		}
		root = node;
		if (node < min || !min)
			min = node;
	}

	static Sp find_min(Sp node)
	{
		if (node)
			while (node->left)
				node = node->left;
		return node;
	}

	Sp get_min() const
	{
		return min;
	}

	// Note: No splay here(?), assuming we'll not do this very often.
	Sp get_max()
	{
		assert(root);
		Sp cur = root;
		while (cur->right) cur = cur->right;
		return cur;
	}

	// NB! Destructively updates 'node' and puts it in either the smaller or
	// bigger tree.
	// TODO Rewrite into something with two return values
	static void partition(Sp pivot, Sp node, Sp* smaller, Sp* bigger)
	{
		if (!node)
		{
			*smaller = *bigger = NULL;
			return;
		}

		Sp a = node->left, b = node->right;
		if (node <= pivot)
		{
			if (b)
			{
				Sp b1 = b->left, b2 = b->right;
				if (b <= pivot)
				{
					//a == node->left already
					//node->left = a;
					node->right = b1;
					b->left = node;
					*smaller = b;
					partition(pivot, b2, &b->right, bigger);
				}
				else
				{
					//a == x->left already
					//x->left = a;
					*smaller = node;
					// likewise, b2 == y->right
					//y->right = b2;
					*bigger = b;
					partition(pivot, b1, &node->right, &b->left);
				}
			}
			else
			{
				*smaller = node;
				*bigger = NULL;
			}
		}
		else
		{
			if (a)
			{
				Sp a1 = a->left, a2 = a->right;
				if (a <= pivot)
				{
					*smaller = a;
					// a1 == a->left
					//a->left = a1;
					*bigger = node;
					// b == node->right
					//node->right = b;

					partition(pivot, a2, &a->right, &node->left);
				}
				else
				{
					*bigger = a;
					a->right = node;
					node->left = a2;
					// b == node->right
					//node->right = b;

					partition(pivot, a1, smaller, &a->left);
				}
			}
			else
			{
				*smaller = NULL;
				*bigger = node;
			}
		}
	}

	void dump_tree()
	{
		xprintf("Splay-heap: root %p min %p\n", root, min);
		dump_inorder(root, 0);
		xprintf("End\n");
	}

	static void dump_inorder(Sp node, int d)
	{
		if (!node)
			xprintf("%02d: NULL\n", d);
		else
		{
			if (node->left)
				dump_inorder(node->left, d + 1);
			xprintf("%02d: %p\n", d, node);
			if (node->right)
				dump_inorder(node->right, d + 1);
		}
	}
};
static splay_node* get_min(const splay_tree& p)
{
	return p.get_min();
}
static void delete_min(splay_tree& p)
{
	p.delete_min();
}
static void insert(splay_tree& t, splay_node* node)
{
	t.insert(node);
	//debug("Inserted %p:\n", node);
	//t.dump_tree();
}
static void remove(splay_tree& t, splay_node* node)
{
	t.remove(node);
}
static void remove_to_end(splay_tree& t, u8* start)
{
	t.remove_to_end((splay_node*)start);
}
#if 0
static void dump_heap(splay_tree& t)
{
	t.dump_tree();
}
#endif
