#run config/fc_ipv6_5tuple.sh

p 1 ping
p 2 ping

p 2 flow add default 3
p 2 flow add ipv6_5tuple 0001:0203:0405:0607:0809:0a0b:0c0d:0e0f 1011:1213:1415:1617:1819:1a1b:1c1d:1e1f 256 257 6 2
p 2 flow ls
