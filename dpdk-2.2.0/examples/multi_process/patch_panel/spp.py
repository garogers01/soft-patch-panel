#!/usr/bin/python
from __future__ import print_function
import sys, getopt, time, select, cmd, Queue
from thread import *
from threading import Thread
from Queue import Queue
from socket import *
import socket

class GrowingList(list):
    def __setitem__(self, index, value):
        if index >= len(self):
            self.extend([None]*(index + 1 - len(self)))
        list.__setitem__(self, index, value)

MAX_SECONDARY = 16

# init 
primary = ''
secondary_list = []
secondary_count = 0

#init primary comm channel 
main2primary = Queue()
primary2main = Queue()

#init secondary comm channel list 
main2sec = GrowingList()
sec2main = GrowingList()

def connectionthread(name, id, conn, m2s, s2m):
	global secondary_list
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
			print (str(e))
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
			print (str(e))
			break
	
	secondary_list.remove(id)
	conn.close()
	#print 'closed thread: ', name, id
	
def acceptthread(sock, main2sec, sec2main):
	global primary
	global secondary_count
	global threads
	global secondary_list
	#print 'secondary thread started'
	#sys.stdout.write(prompt)
	
	try:
		while True:
			#Accepting incoming connections
			conn, addr = sock.accept()
			#print 'secondary connection from', addr
			#Creating new thread. Calling secondarythread function for this function and passing conn as argument.
			
			#start_new_thread(connectionthread,('secondary', secondary_count,conn,threads[secondary_count],)) 
			#print "init secondary comm channel for id: ", secondary_count
			secondary_list.append(secondary_count)
			main2sec[secondary_count] = Queue()
			sec2main[secondary_count] = Queue()
			start_new_thread(connectionthread,('secondary', secondary_count,conn,main2sec[secondary_count],sec2main[secondary_count],)) 
			secondary_count+=1
			#start new thread takes 1st argument as a function name to be run, second is the tuple of arguments to the function.
	except Exception, e:		
		print (str(e))
		sock.close()

def command_primary(command):
	global main2primary
	global primary2main
	global primary
	
	if primary:
		main2primary.put(command)
		print (primary2main.get(True))
	else:
		print ("primary not started")

def command_secondary(sec_id, command):
	global secondary_list
	if sec_id in secondary_list:
		main2sec[sec_id].put(command)
		print (sec2main[sec_id].get(True))
	else:
		print ("secondary id %d not exist %d" % sec_id)
		
def print_status():
	global primary
	global secondary_list
	print ("Soft Patch Panel Status :")
	print ("primary: %d" % primary)
	print ("secondary count: %d" % len(secondary_list))
	for i in secondary_list:
		print ("Connected secondary id: %d" % i)

def primarythread(sock, main2primary, primary2main):
	global primary
	cmd = ''
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
				print (str(e))
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
				print (str(e))
				break
			
	print ("primary communication thread end")

		
class Shell(cmd.Cmd):
	intro = 'Welcome to the spp.   Type help or ? to list commands.\n'
	prompt = 'spp > '
	file = None
	
	FRIENDS = [ 'Alice', 'Adam', 'Barbara', 'Bob' ]
	COMMANDS = [ 'status', 'add', 'patch', 'ring', 'vhost', 'reset', 'exit','forward']
    
	def do_greet(self, person):
		"Greet the person"
		if person and person in self.FRIENDS:
			greeting = 'hi, %s!' % person
		elif person:
			greeting = "hello, " + person
		else:
			greeting = 'hello'
		print (greeting)
    
	def complete_greet(self, text, line, begidx, endidx):
		if not text:
			completions = self.FRIENDS[:]
		else:
			completions = [ f
							for f in self.FRIENDS
							if f.startswith(text)
							]
		return completions

	def complete_pri(self, text, line, begidx, endidx):
		if not text:
			completions = self.COMMANDS[:]
		else:
			completions = [ p
							for p in self.COMMANDS
							if p.startswith(text)
							]
		return completions		

	def complete_sec(self, text, line, begidx, endidx):
		if not text:
			completions = self.COMMANDS[:]
		else:
			completions = [ p
							for p in self.COMMANDS
							if p.startswith(text)
							]
		return completions		
		
	# ----- display spp status -----
	def do_status(self, line):
		"Display Soft Patch Panel Status"
		print_status()

	def do_pri(self, command):
		"send command to primary"
		if command and command in self.COMMANDS:
			command_primary(command)
		else:
			print ("primary invalid command")

	def do_sec(self, arg):
		"send command to secondary"
		cmds = arg.split(';')
		if len(cmds) < 2:
			print ("error")
		elif str.isdigit(cmds[0]):
			sec_id = int (cmds[0])
			command_secondary(sec_id, cmds[1])
		else:
			print (cmds[0])
			print ("first %s" % cmds[1])
	
	# ----- record and playback -----
	def do_record(self, arg):
		'Save future commands to filename:  RECORD filename.cmd'
		self.file = open(arg, 'w')
	def do_playback(self, arg):
		'Playback commands from a file:  PLAYBACK filename.cmd'
		self.close()
		with open(arg) as file:
			self.cmdqueue.extend(file.read().splitlines())			
	def precmd(self, line):
		line = line.lower()
		if self.file and 'playback' not in line:
			print(line, file=self.file)
		return line
	def close(self):
		if self.file:
			print("closing file")
			self.file.close()
			self.file = None
			
	def do_bye(self, arg):
		'Stop recording, close SPP, and exit:  BYE'
		print('Thank you for using Soft Patch Panel')
		self.close()
		return True
		
def main(argv):
	# Defining server address and port
	host = ''  #'localhost' or '127.0.0.1' or '' are all same
	port = 52000
	
	try:
		opts, args = getopt.getopt(argv,"p:s:h",["help","primary=","secondary"])
	except getopt.GetoptError:
		print ('spp.py -p <primary__port_number> -s <secondary_port_number>')
		sys.exit(2)
	for opt, arg in opts:
		#print 'opt is ', opt
		#print 'arg is ', arg
		if opt in ("-h", "--help"):
			print ('spp.py -p <primary__port_number> -s <secondary_port_number>')
			sys.exit()
		elif opt in ("-p", "--primary"):
			primary_port = int(arg)
			print ("primary port : %d" % primary_port)	
		elif opt in ("-s", "--secondary"):
			secondary_port = int(arg)
			print ('secondary port : %d' % secondary_port)

	#Creating primary socket object
	primary_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	#Binding primary socket to a address. bind() takes tuple of host and port.
	primary_sock.bind((host, primary_port))
	#Listening primary at the address
	primary_sock.listen(1) #5 denotes the number of clients can queue				
	
	primary_thread = Thread(target=primarythread, args=(primary_sock, main2primary, primary2main,))
	primary_thread.daemon = True
	primary_thread.start()

	#Creating secondary socket object
	secondary_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	#Binding secondary socket to a address. bind() takes tuple of host and port.
	secondary_sock.bind((host, secondary_port))
	#Listening secondary at the address
	#print 'MAX_SECONDARY : ', MAX_SECONDARY
	secondary_sock.listen(MAX_SECONDARY) #5 denotes the number of clients can queue
	
	# secondary process handling thread
	start_new_thread(acceptthread,(secondary_sock, main2sec, sec2main))
	
	shell = Shell()
	shell.cmdloop()
	shell = None
	
	primary_sock.shutdown(SHUT_RDWR)
	primary_sock.close()
	secondary_sock.shutdown(SHUT_RDWR)
	secondary_sock.close()		
			
if __name__ == "__main__":
	main(sys.argv[1:])