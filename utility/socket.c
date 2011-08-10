#include "socket.h"
#include "net.h"
#include "enc28j60.h"
#include "ip_arp_udp_tcp.h"
#define BUFFER_SIZE         800
#define MAX_ITERATIONS      1000

#define NO_STATE            0
#define GOT_MAC             1
#define ARP_REQUEST_SENT    2
#define TCP_SYN_SENT        3

uint8_t myMacAddress[6], myIpAddress[4], myGatewayIpAddress[4],
        mySubnetAddress[4];
static uint8_t buffer[BUFFER_SIZE + 1];
uint16_t packetLength;
#ifdef ETHERSHIELD_DEBUG
static void serial_write(unsigned char c) {
    while (!(UCSR0A & (1 << UDRE0))) {}
    UDR0 = c;
}

void ethershieldDebug(char *message) {
    uint8_t i;
    for (i = 0; message[i] != '\0'; i++) {
        serial_write(message[i]);
    }
}
char *debugCode2String(uint8_t debugCode) {
    if (debugCode == DEBUG_RECEIVED_ARP_REPLY) {
        return "RECEIVED_ARP_REPLY";
    }
    else if (debugCode == DEBUG_ANSWERING_RECEIVED_ARP_REQUEST) {
        return "ANSWERING_RECEIVED_ARP_REQUEST";
    }
    else if (debugCode == DEBUG_IGNORING_PACKET_NOT_FOR_ME) {
        return "IGNORING_PACKET_NOT_FOR_ME";
    }
    else if (debugCode == DEBUG_REPLYING_ECHO_REQUEST) {
        return "REPLYING_ECHO_REQUEST";
    }
    else if (debugCode == DEBUG_IGNORED_PACKET) {
        return "IGNORED_PACKET";
    }
    else if (debugCode == DEBUG_RECEIVED_TCP_SYNACK_SENDING_ACK) {
        return "RECEIVED_TCP_SYNACK_SENDING_ACK";
    }
    else if (debugCode == DEBUG_RECEIVED_TCP_SYN_SENDING_SYNACK) {
        return "RECEIVED_TCP_SYN_SENDING_SYNACK";
    }
    else if (debugCode == DEBUG_RECEIVED_ACKFIN_CLOSING_SOCKET) {
        return "RECEIVED_ACKFIN_CLOSING_SOCKET";
    }
    else if (debugCode == DEBUG_RECEIVED_ACK_PACKET_HAVE_NO_DATA) {
        return "RECEIVED_ACK_PACKET_HAVE_NO_DATA";
    }
    else if (debugCode == DEBUG_RECEIVED_ACK_PACKET_WITH_DATA) {
        return "RECEIVED_ACK_PACKET_WITH_DATA";
    }
    else if (debugCode == DEBUG_DONT_KNOW_WHAT_TO_DO) {
        return "DONT_KNOW_WHAT_TO_DO";
    }
    else if (debugCode == DEBUG_SENT_ARP_REQUEST) {
        return "SENT_ARP_REQUEST";
    }
    else if (debugCode == DEBUG_MAC_RECEIVED_SENDING_TCP_SYN) {
        return "MAC_RECEIVED_SENDING_TCP_SYN";
    }
    else if (debugCode == DEBUG_TCP_SYN_SENT) {
        return "TCP_SYN_SENT";
    }
    else if (debugCode == DEBUG_INIT) {
        return "INIT";
    }
}
#endif

typedef struct socketData {
    uint8_t protocol;
    uint16_t sourcePort;
    uint8_t flag;
    uint8_t state;
    uint8_t *dataBuffer;
    uint16_t bytesToRead;
    uint16_t firstByte;
    uint8_t clientState;
    uint8_t destinationMac[6];
    uint16_t sendPacketLength;
} SocketData;
SocketData _SOCKETS[MAX_SOCK_NUM];

#ifdef ETHERSHIELD_DEBUG
void turnLEDsOn() {
    enc28j60PhyWrite(PHLCON, 0x880); //turn on
}

void turnLEDsOff() {
    enc28j60PhyWrite(PHLCON, 0x990); //turn off
}
#endif

uint8_t socket(SOCKET s, uint8_t protocol, uint16_t sourcePort, uint8_t flag) {
    _SOCKETS[s].protocol = protocol;
    _SOCKETS[s].sourcePort = sourcePort;
    _SOCKETS[s].flag = flag;
    _SOCKETS[s].state = SOCK_INIT;
    _SOCKETS[s].bytesToRead = 0;
    _SOCKETS[s].firstByte = 0;
    _SOCKETS[s].clientState = NO_STATE;
}

void flushSockets() {
    packetLength = enc28j60PacketReceive(BUFFER_SIZE, buffer);
    if (!packetLength) {
        //DEBUG: no data available for reading!
        return;
    }
    else if (eth_type_is_arp_and_my_ip(buffer, packetLength)) {
        if (arp_packet_is_myreply_arp(buffer)) {
            uint8_t i;
#ifdef ETHERSHIELD_DEBUG
            ethershieldDebug("Received ARP reply.\r\n");
#endif
            for (i = 0; i < 6; i++) {
                _SOCKETS[0].destinationMac[i] = buffer[ETH_SRC_MAC + i];
            }
            _SOCKETS[0].clientState = GOT_MAC;
            //TODO: write code to get socket id instead of 0
        }
        else {
#ifdef ETHERSHIELD_DEBUG
            ethershieldDebug("Answering ARP request.\r\n");
#endif
            make_arp_answer_from_request(buffer);
        }
    }
    else if (!eth_type_is_ip_and_my_ip(buffer, packetLength)) {
#ifdef ETHERSHIELD_DEBUG
        ethershieldDebug("Ignoring packet not for me.\r\n");
#endif
        return;
    }
    else if (buffer[IP_PROTO_P] == IP_PROTO_ICMP_V &&
            buffer[ICMP_TYPE_P] == ICMP_TYPE_ECHOREQUEST_V) {
#ifdef ETHERSHIELD_DEBUG
        //char myStr[80];
        //sprintf(myStr, "Replying ECHO REQUEST from %d.%d.%d.%d",
        //        buffer[IP_SRC_IP_P], buffer[IP_SRC_IP_P + 1],
        //        buffer[IP_SRC_IP_P + 2], buffer[IP_SRC_IP_P + 3]);
        ethershieldDebug("Replying ARP ECHO request.\r\n");
#endif
        make_echo_reply_from_request(buffer, packetLength);
    }
    else if (buffer[IP_PROTO_P] == IP_PROTO_TCP_V) {
        //DEBUG: it's TCP and for me! Do I want it?
        uint16_t destinationPort = (buffer[TCP_DST_PORT_H_P] << 8) | buffer[TCP_DST_PORT_L_P];
        uint8_t i, socketSelected = 255;
        for (i = 0; i < MAX_SOCK_NUM; i++) {
            if (_SOCKETS[i].sourcePort == destinationPort &&
                _SOCKETS[i].state == SOCK_LISTEN) {
                socketSelected = i;
                break;
            }
        }
        if (socketSelected == 255) {
#ifdef ETHERSHIELD_DEBUG
            //char myStr[80];
            //sprintf(myStr, "Packet from: %d.%d.%d.%d:%d ignored.",
            //        buffer[IP_SRC_IP_P], buffer[IP_SRC_IP_P + 1],
            //        buffer[IP_SRC_IP_P + 2], buffer[IP_SRC_IP_P + 3],
            //        destinationPort);
            ethershieldDebug("Packet ignored.\r\n");
#endif
            //return; //TODO: return!
            socketSelected = 0; //TODO: remove this hack
        }
        //TODO: change next 'if' to 'else if'
        //DEBUG: ok, the TCP packet is for me and I want it.
        if (buffer[TCP_FLAGS_P] == (TCP_FLAG_SYN_V | TCP_FLAG_ACK_V)) {
#ifdef ETHERSHIELD_DEBUG
            ethershieldDebug("Received TCP SYNACK, sending ACK.\r\n");
#endif
            make_tcp_ack_from_any(buffer);
            //TODO: verify if I'm waiting for this SYN+ACK
            _SOCKETS[socketSelected].clientState = SOCK_ESTABLISHED;
            //TODO: write code to get socket id instead of 0
            return;
        }
        else if (buffer[TCP_FLAGS_P] & TCP_FLAGS_SYN_V) {
#ifdef ETHERSHIELD_DEBUG
            ethershieldDebug("Received TCP SYN, sending SYNACK.\r\n");
#endif
            _SOCKETS[socketSelected].state = SOCK_ESTABLISHED;
            make_tcp_synack_from_syn(buffer);
        }
        else if (buffer[TCP_FLAGS_P] & TCP_FLAGS_ACK_V) {
            uint16_t data;
            init_len_info(buffer);
            data = get_tcp_data_pointer();
            if (!data) {
#ifdef ETHERSHIELD_DEBUG
                ethershieldDebug("Received ACK pkt with no data.\r\n");
#endif
                if (buffer[TCP_FLAGS_P] & TCP_FLAGS_FIN_V) {
#ifdef ETHERSHIELD_DEBUG
                    ethershieldDebug("Received ACKFIN, closing socket.\r\n");
#endif
                    make_tcp_ack_from_any(buffer);
                    _SOCKETS[socketSelected].state = SOCK_CLOSED;
                    _SOCKETS[socketSelected].sendPacketLength = 0;
                    free(_SOCKETS[socketSelected].dataBuffer);
                }
                return;
            }
            else {
                int i, dataSize;
		make_tcp_ack_from_any(buffer); //TODO-ACK
                dataSize = packetLength - (&buffer[data] - buffer);
#ifdef ETHERSHIELD_DEBUG
                char value[6];
                itoa(dataSize, value, 10);
                ethershieldDebug("Received ACK pkt with data, ACK sent.\r\n");
                ethershieldDebug("  # bytes: ");
                ethershieldDebug(value);
                ethershieldDebug("\r\n");
#endif
                _SOCKETS[socketSelected].state = SOCK_ESTABLISHED;
                _SOCKETS[socketSelected].dataBuffer = malloc(dataSize * sizeof(char)); //TODO: and about the TCP/IP/Ethernet overhead?
                for (i = 0; i < dataSize; i++) {
                    _SOCKETS[socketSelected].dataBuffer[i] = buffer[data + i];
                }
                _SOCKETS[socketSelected].bytesToRead = i;
#ifdef ETHERSHIELD_DEBUG
                ethershieldDebug("  Data:\r\n");
                for (i = 0; i < dataSize; i++) {
                    serial_write(_SOCKETS[socketSelected].dataBuffer[i]);
                }
                ethershieldDebug("\r\n");
#endif
                return;
            }
        }
        else {
#ifdef ETHERSHIELD_DEBUG
            ethershieldDebug("Don't know what to do!\r\n");
#endif
            //make_tcp_ack_from_any(buffer); //TODO-ACK: send ACK using tcp_client_send_packet
        }
    }
}

uint8_t listen(SOCKET s) {
    _SOCKETS[s].state = SOCK_LISTEN;
    return 1;
}

uint8_t connect(SOCKET s, uint8_t *destinationIp, uint16_t destinationPort) {
    uint16_t packetChecksum, i, sourcePort;
    char buffer[59];
#ifdef ETHERSHIELD_DEBUG
    char myStr[80];
#endif

    //TODO: create an ARP table?
    make_arp_request(buffer, destinationIp);
    _SOCKETS[s].clientState = ARP_REQUEST_SENT;
#ifdef ETHERSHIELD_DEBUG
    ethershieldDebug("Sent ARP request.\r\n");
#endif

    for (i = 0; _SOCKETS[s].clientState != GOT_MAC && i < MAX_ITERATIONS; i++) {
        flushSockets(); //it'll fill destinationMac on socket struct
    }
    if (_SOCKETS[s].clientState != GOT_MAC) {
        return 0;
    }
#ifdef ETHERSHIELD_DEBUG
    ethershieldDebug("MAC received, sending TCP SYN.\r\n");
#endif

    sourcePort = 42845; //TODO: change this
    tcp_client_send_packet(buffer, destinationPort, sourcePort, TCP_FLAG_SYN_V,
            1, 1, 0, 0, _SOCKETS[s].destinationMac,
            destinationIp);
    _SOCKETS[s].clientState = TCP_SYN_SENT;
#ifdef ETHERSHIELD_DEBUG
    ethershieldDebug("TCP SYN sent.\r\n");
#endif

    for (i = 0; _SOCKETS[s].clientState != SOCK_ESTABLISHED && i < MAX_ITERATIONS; i++) {
        flushSockets();
    }

    return _SOCKETS[s].clientState == SOCK_ESTABLISHED;
    //TODO: Maybe use a timeout instead of MAX_ITERATIONS to receive SYN+ACK
}

uint16_t send(SOCKET s, const uint8_t *bufferToSend, uint16_t length) {
    _SOCKETS[s].sendPacketLength = fill_tcp_data2(buffer, _SOCKETS[s].sendPacketLength, bufferToSend, length);
} //TODO: do it per socket

uint16_t recv(SOCKET s, uint8_t *recvBuffer, uint16_t length) {
    if (_SOCKETS[s].bytesToRead == 0) {
        return;
    }
    else if (length == 1) {
        recvBuffer[0] = _SOCKETS[s].dataBuffer[_SOCKETS[s].firstByte];
        _SOCKETS[s].firstByte++;
        if (_SOCKETS[s].firstByte == _SOCKETS[s].bytesToRead) {
            _SOCKETS[s].bytesToRead = 0;
            _SOCKETS[s].firstByte = 0;
            free(_SOCKETS[s].dataBuffer);
        }
    }
    else {
        //TODO: what if length > 1?
    }
}

uint8_t disconnect(SOCKET s) {
    if (_SOCKETS[s].sendPacketLength) {
        //make_tcp_ack_from_any(buffer); //TODO-ACK
        make_tcp_ack_with_data(buffer, _SOCKETS[s].sendPacketLength);
        _SOCKETS[s].sendPacketLength = 0;
    }
    //TODO: send FYN packet
    //TODO: wait to receive ACK?
    close(s);
}

uint8_t close(SOCKET s) {
    //do not call the function that does verifications
    _SOCKETS[s].state = SOCK_CLOSED;
    _SOCKETS[s].sendPacketLength = 0;
    //free(_SOCKETS[s].dataBuffer); //TODO: really need this?
}

uint8_t getSn_SR(SOCKET s) {
    //get socket status
    //TODO: change on standard Ethernet library to do not use this

    flushSockets();
    return _SOCKETS[s].state;
}

uint16_t getSn_RX_RSR(SOCKET s) {
    //return the size of the receive buffer for that socket
    //TODO: change on standard Ethernet library to do not use this

    flushSockets();
    return _SOCKETS[s].bytesToRead;
}

void iinchip_init() {
    //TODO: change on standard Ethernet library to do not use this

    //do nothing
}

void sysinit(uint8_t txSize, uint8_t rxSize) {
    //TODO: change on standard Ethernet library to do not use this
    uint8_t i;
    for (i = 0; i < MAX_SOCK_NUM; i++) {
        _SOCKETS[i].state = SOCK_CLOSED;
        _SOCKETS[i].sendPacketLength = 0;
    }
#ifdef ETHERSHIELD_DEBUG
    ethershieldDebug("Init.\r\n");
#endif
}

void setSHAR(uint8_t *macAddress) {
    //TODO: change on standard Ethernet library to do not use this
    uint8_t i;
    for (i = 0; i < 6; i++) {
        myMacAddress[i] = macAddress[i];
    }
    enc28j60Init(myMacAddress);
    enc28j60clkout(2);
    enc28j60PhyWrite(PHLCON, 0x476); //LEDA = link status, LEDB = RX/TX
}

void setSIPR(uint8_t *ipAddress) {
    //TODO: change on standard Ethernet library to do not use this
    uint8_t i;
    for (i = 0; i < 4; i++) {
        myIpAddress[i] = ipAddress[i];
    }
}

void setGAR(uint8_t *gatewayIpAddress) {
    //TODO: change on standard Ethernet library to do not use this
    uint8_t i;
    for (i = 0; i < 4; i++) {
        myGatewayIpAddress[i] = gatewayIpAddress[i];
    }
}

void setSUBR(uint8_t *subnetAddress) {
    //TODO: change on standard Ethernet library to do not use this
    uint8_t i;
    for (i = 0; i < 4; i++) {
        mySubnetAddress[i] = subnetAddress[i];
    }

    init_ip_arp_udp_tcp(myMacAddress, myIpAddress);
}
