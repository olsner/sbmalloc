import sys

allocated = set()

for x in sys.stdin.readlines():
	if x[0] != 'X': continue

	try:
		_,c,p = x.split()
	except ValueError:
		continue
	print repr(c),'/',repr(p), len(allocated)
	p = eval(p)
	if c == 'ALLOC':
		assert p not in allocated
		allocated.add(p)
	elif c == 'FREE':
		assert p in allocated
		allocated.remove(p)
	else:
		assert False
	
print len(allocated)
