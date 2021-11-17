/********************************************************************
 *
 *	Copyright (C) 2021  John E. Wulff
 *
 *  You may distribute under the terms of either the GNU General Public
 *  License or the Artistic License, as specified in the README file.
 *
 *  For more information about this program, or for information on how
 *  to contact the author, see the README file
 *
 *	Package tcpcomm
 *	TCP/IC communication support
 *
 *  Read and Write buffers match the buffers used in Perl module msg.pm
 *  The contents of the buffers consists of a 4 byte length (BigEndian)
 *  followed by the number of bytes specified by that length.
 *  The maximum number of bytes is 1400 for TCP.
 *
 *******************************************************************/

package tcpcomm

import (
	"io"
	"net"
	"encoding/binary"
	"fmt"
)

const ID_tcpcomm_go = "$Id: tcpcomm.go 1.1 $"
const (
    REPLY	= 1400			// max size of TCP reply, in bytes
)

/********************************************************************
 *
 *  Read a TCP message into Read buffer rbuf
 *  Local []byte slices lbuf and rbuf are on a different stack for
 *  each call from a different goroutine and hence concurrency safe
 *
 *******************************************************************/

func Read(conn net.Conn) ([]byte, int, error) {
    var err error
    lbuf := make([]byte, 4)
    if _, err = io.ReadFull(conn, lbuf); err != nil {
	return lbuf, 0, err
    }
    length := int(binary.BigEndian.Uint32(lbuf))
    rbuf := make([]byte, length)
    if _, err := io.ReadFull(conn, rbuf); err != nil {
	return rbuf, 0, err
    }
    return rbuf, length, err
}

/********************************************************************
 *
 *  Write a TCP message of a specified length from Write buffer pointed
 *  to by wbufP. The buffer is declared and filled in the calling
 *  goroutine, which makes it concurrency safe
 *
 *******************************************************************/

type lengthError struct{}

func (m *lengthError) Error() string {
    return fmt.Sprintf("tcpcomm.Write: message length is too long (> %d)", REPLY)
}

func Write(conn net.Conn, wbufP *[]byte, length int) (error) {
    if length > REPLY {
	return &lengthError{};
    }
    binary.BigEndian.PutUint32((*wbufP)[:4], uint32(length))
    _, err := conn.Write((*wbufP)[:length+4])
    return err
}
