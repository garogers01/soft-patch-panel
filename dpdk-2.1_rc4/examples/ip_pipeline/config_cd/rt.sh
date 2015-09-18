#run config/routing.sh

p 1 ping

p 1 arp add default 2
p 1 arp add 0 10.0.0.1 a0:b0:c0:d0:e0:f0
p 1 arp add 1 11.0.0.1 a1:b1:c1:d1:e1:f1
p 1 arp add 2 12.0.0.1 a2:b2:c2:d2:e2:f2
p 1 arp add 3 13.0.0.1 a3:b3:c3:d3:e3:f3

p 1 route add default 3
p 1 route add 0.0.0.0 10 0 10.0.0.1
p 1 route add 0.64.0.0 10 1 11.0.0.1
p 1 route add 0.128.0.0 10 2 12.0.0.1
p 1 route add 0.192.0.0 10 3 13.0.0.1

p 1 route ls
p 1 arp ls
