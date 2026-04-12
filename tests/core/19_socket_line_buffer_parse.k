# Test socket line buffer parser helper used by receiveLineTimeout

from socket import Socket;

var s = Socket()
s._rxBuffer = "MSG|a|one\nMSG|b|two\n"
print(s._tryPopLineFromBuffer())
print(s._tryPopLineFromBuffer())
print(type(s._tryPopLineFromBuffer()) == "null")

s._rxBuffer = "SYS|bye\r\n"
print(s._tryPopLineFromBuffer())

# EXPECTED
# MSG|a|one
# MSG|b|two
# true
# SYS|bye
# END_EXPECTED
