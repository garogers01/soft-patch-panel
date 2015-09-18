#Firewall

p 1 firewall add ipv4 1 0.0.0.0 0 0.0.0.0 10 0 65535 0 65535 6 0xf 0
p 1 firewall add ipv4 1 0.0.0.0 0 0.64.0.0 10 0 65535 0 65535 6 0xf 1
p 1 firewall add ipv4 1 0.0.0.0 0 0.128.0.0 10 0 65535 0 65535 6 0xf 2
p 1 firewall add ipv4 1 0.0.0.0 0 0.192.0.0 10 0 65535 0 65535 6 0xf 3

p 1 firewall ls

#p 1 firewall del ipv4 0.0.0.0 0 0.0.0.0 10 0 65535 0 65535 6 0xf
#p 1 firewall del ipv4 0.0.0.0 0 0.64.0.0 10 0 65535 0 65535 6 0xf
#p 1 firewall del ipv4 0.0.0.0 0 0.128.0.0 10 0 65535 0 65535 6 0xf
#p 1 firewall del ipv4 0.0.0.0 0 0.192.0.0 10 0 65535 0 65535 6 0xf
