#run config/fc_qinq.sh

p 1 ping
p 2 ping

p 2 flow add default 3
p 2 flow add qinq 256 257 2
p 2 flow ls
