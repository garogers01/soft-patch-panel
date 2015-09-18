#run config/fc_ipv4_5tuple.sh

p 1 ping
p 2 ping

p 2 flow add default 3
p 2 flow add ipv4_5tuple 1.2.3.4 5.6.7.8 256 257 6 2
p 2 flow ls

