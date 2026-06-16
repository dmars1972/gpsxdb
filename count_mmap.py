#!/home/daniel/py-nogil/bin/python3

import struct

total = 0
populated = 0

with open('nodes.dat', 'rb') as f:
	while True:
		val = f.read(16)
		if not val:
			break
		s = struct.unpack('dd', val)


		if (s[0], s[1]) != (0.0,0.0):
			populated += 1
		total += 1
		if total % 10000 == 0:
			print(f'Total: {total}, Populated {populated}', end='\r')

print(f'Total: {total}, Populated {populated}')
