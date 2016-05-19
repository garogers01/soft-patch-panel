#!/usr/bin/python

import sys, getopt, time, select
# Import all from module socket
from socket import *
import socket
#Importing all from thread
from thread import *
from threading import Thread
#Importing all from queue inter thread communication
from Queue import Queue

threads = []
seclist = []
thread_count = 0
secondary_count = 0
sec_count = 0
prompt = 'main > '

primary = False
MAX_SECONDARY = 16

def connectionthread(name, id, conn, m2s, s2m):
	global seclist
	global secondary_count
	#print 'start thread: ', name, id
	cmd = 'hello'

	#infinite loop so that function do not terminate and thread do not end.	
	while True:
		try:
			ready_to_read, ready_to_write, in_error = \
				select.select([conn,], [conn,], [], 5)
		except select.error:
			break
		#Sending message to connected secondary
		try:
			cmd = m2s.get(True)
			#print 'secondary thread: cmd', cmd
			conn.send(cmd) #send only takes string
		except KeyError:
			break
		except Exception, e:
			print str(e) 
			break
		#Receiving from secondary
		try:
			data = conn.recv(1024) # 1024 stands for bytes of data to be received
			if data:
				#print "primary reply: ", data
				s2m.put("recv:" + str(conn.fileno()) + ":" + "{" + data + "}")
			else:
				s2m.put("closing:" + str(conn))
				#print " closing", conn
				break
		except Exception, e:
			print str(e)
			break
	
	seclist.remove(id)
	conn.close()
	#print 'closed thread: ', name, id
	
def acceptthread(sock, main2sec, sec2main):
	global primary
	global secondary_count
	global threads
	global seclist
	#print 'secondary thread started'
	#sys.stdout.write(prompt)
	while True:
		#Accepting incoming connections
		conn, addr = sock.accept()
		#print 'secondary connection from', addr
		#Creating new thread. Calling secondarythread function for this function and passing conn as argument.
		
		#start_new_thread(connectionthread,('secondary', secondary_count,conn,threads[secondary_count],)) 
		#print "init secondary comm channel for id: ", secondary_count
		seclist.append(secondary_count)
		main2sec[secondary_count] = Queue()
		sec2main[secondary_count] = Queue()
		start_new_thread(connectionthread,('secondary', secondary_count,conn,main2sec[secondary_count],sec2main[secondary_count],)) 
		secondary_count+=1
		#start new thread takes 1st argument as a function name to be run, second is the tuple of arguments to the function.
	
	sock.close()

def primarythread(sock, main2primary, primary2main):
	cmd = ''
	global primary
	#print 'primary thread started'
	
	while True:
		#waiting for connection
		primary = False
		conn, addr = sock.accept()
		#print 'primary connection from', addr
		primary = True
		
		while conn:
			try:
				ready_to_read, ready_to_write, in_error = \
					select.select([conn,], [conn,], [], 5)
			except select.error:
				break
			#Sending message to connected primary
			try:
				cmd = main2primary.get(True)
				#print 'primary thread: cmd', cmd
				conn.send(cmd) #send only takes string
			except KeyError:
				break
			except Exception, e:
				print str(e) 
				break
			#Receiving from primary
			try:
				data = conn.recv(1024) # 1024 stands for bytes of data to be received
				if data:
					#print "primary reply: ", data
					primary2main.put("recv:" + str(addr) + ":" + "{" + data + "}")
				else:
					primary2main.put("closing:" + str(addr))
					#print " closing", addr
					conn.close()
					break
			except Exception, e:
				print str(e)
				break
			
	print "primary communication thread end"	
	
def obsoletethread(sock, message_queues):	
	sockname = sock.getsockname() 
	inputs = [sock]
	outputs = []
	global primary

	primary = False
	
	while sock:
		print "waiting for next event"
		readable, writable, exceptional = select(inputs,outputs,inputs)		
		for s in writable:
			print "s in writable"
			if s is sock:
				print "get command from queue"
				s.send(message_queues[s].get(True))
			else:
				print "error"			
		for s in readable:
			print "s in readable"
			if s is sock:
				#A "readable" socket is ready to accept a connection
				connection,client_address=s.accept()
				print "  connection from",client_address
				connection.setblocking(0)
				primary = True
			else:
				print "s receive"
				data = s.recv(4096)
				if data:
					print ("recv:" + str(sockname) + ":" + "{" + data + "}")
					#Add output  channel for response
					if s not in outputs:
						outputs.append(s)
				else:
					#Interpret empty result as closed connection
					print " closing", client_address
					if s in outputs:
						outputs.remove(s)
					inputs.remove(s)
					s.close()
					#remove message queue
					#del message_queues[s]
		for s in exceptional:
			print " exception condition on ",s.getpeername()
			#stop listening for input on the connection
			inputs.remove(s)
			if s in outputs:
				outputs.remove(s)
			s.close()
			primary = False
			
	print "primary thread exit"
			
class GrowingList(list):
    def __setitem__(self, index, value):
        if index >= len(self):
            self.extend([None]*(index + 1 - len(self)))
        list.__setitem__(self, index, value)
	
def main(argv):
	# Defining server address and port
	host = ''  #'localhost' or '127.0.0.1' or '' are all same
	port = 52000
	input = ''
	thread_id = 9999 # Invalid
	cmd = ''
	thread_queue = ''
	secondarylist = ''
	main2sec = GrowingList()
	sec2main = GrowingList()
	
	#init primary comm channel 
	main2primary = Queue()
	primary2main = Queue()
	global primary
	
	#init secondary data
	global seclist

	try:
		opts, args = getopt.getopt(argv,"h:p:",["port="])
	except getopt.GetoptError:
		print 'controller.py -p <port_number>'
		sys.exit(2)
	for opt, arg in opts:
		print 'opt is ', opt
		print 'arg is ', arg
		if opt in ("-h", "--help"):
			print 'controller.py -p <port_number>'
			sys.exit()
		elif opt in ("-p", "--port"):
			port = int(arg)
			print 'port is ', port

	#Creating primary socket object
	primary_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	#Binding primary socket to a address. bind() takes tuple of host and port.
	primary_sock.bind((host, port-1))
	#Listening primary at the address
	primary_sock.listen(1) #5 denotes the number of clients can queue	
	
	primary_thread = Thread(target=primarythread, args=(primary_sock, main2primary, primary2main,))
	primary_thread.daemon = True
	primary_thread.start()
	#start_new_thread(primarythread,(primary_sock, main2primary, primary2main)) 
			
	#Creating secondary socket object
	secondary_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	#Binding secondary socket to a address. bind() takes tuple of host and port.
	secondary_sock.bind((host, port))
	#Listening secondary at the address
	secondary_sock.listen(16) #5 denotes the number of clients can queue
	
	start_new_thread(acceptthread,(secondary_sock, main2sec, sec2main)) 


	while True:
		try:
			time.sleep(0.1)
			input = raw_input(prompt)
		except (KeyError, KeyboardInterrupt) as e:
			print "exit" 
			break
		if input == 'h' or input == '?':
			print '<secondary id>;<command>'
			continue
		outputs = input.split ('|')	
		for output in outputs:
			cmds = output.split (';', 1)	
			if len (output) < 2:
				continue
			elif cmds[0] == 'status':
				print "primary: ", primary
				print "secondary count: ", len(seclist)
				for i in seclist:
					print "Connected secondary id:" , i
			elif cmds[0] == 'pp':
				if primary:
					main2primary.put(cmds[1])
					print primary2main.get(True)
				else:
					print "primary not started"
			elif str.isdigit(cmds[0]):
				thread_id = int (cmds[0])
				tempset = set (seclist)
				if thread_id in tempset:
					main2sec[thread_id].put(cmds[1])
					print sec2main[thread_id].get(True)
				else:
					print "secondary id %d not exist" % thread_id
			elif cmds[0] == 'exit': 
				primary_sock.shutdown(SHUT_RDWR)
				primary_sock.close()
				secondary_sock.shutdown(SHUT_RDWR)
				secondary_sock.close()
				sys.exit()
			else:
				print "invalid command"
		if cmds[0] == 'exit':
			print "exiting main 2"
			break
			
	
if __name__ == "__main__":
   main(sys.argv[1:])
