PROGRAMS=mailgrave-queue mailgrave-send mailgrave-inject mailgrave-remote \
	 mailgrave-smtpd
TESTS=rfc822-address

all: $(PROGRAMS)

test: $(TESTS)
	./rfc822-address

clean:
	rm -f $(PROGRAMS) $(TESTS) *~ DEADJOE status 0000*dat 0000*env queue.ctrl

mailgrave-queue: mailgrave-queue.cc status.cc createsocket.cc createsocket.hh \
		 opensocket.cc opensocket.hh cug.cc cug.hh
	g++ -Wall -g -o mailgrave-queue mailgrave-queue.cc status.cc createsocket.cc opensocket.cc cug.cc

mailgrave-smtpd: mailgrave-smtpd.cc cug.cc cug.hh
	g++ -Wall -g -o mailgrave-smtpd mailgrave-smtpd.cc cug.cc

mailgrave-send: mailgrave-send.cc status.cc createsocket.cc createsocket.hh opensocket.cc opensocket.hh cug.cc cug.hh
	g++ -Wall -g -o mailgrave-send mailgrave-send.cc status.cc createsocket.cc opensocket.cc cug.cc

mailgrave-inject: mailgrave-inject.cc rfc822-address.cc opensocket.cc opensocket.hh
	g++ -Wall -g -o mailgrave-inject mailgrave-inject.cc rfc822-address.cc opensocket.cc

mailgrave-remote: mailgrave-remote.cc createsocket.cc createsocket.hh cug.cc cug.hh
	g++ -Wall -g -o mailgrave-remote mailgrave-remote.cc createsocket.cc cug.cc

rfc822-address: rfc822-address.cc
	g++ -DTEST -Wall -g -o rfc822-address rfc822-address.cc
