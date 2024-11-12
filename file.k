import "lang.k";

var F_RO = 0
var F_WO = 1
var F_RW = 2
var F_CR = 64
var F_TR = 512

var U_READ  = 256 # Read by user.  
var U_WRITE = 128 # Write by user.  
var U_EXEC  = 64  # Execute by user. 

var G_READ  = 32 # Read by groups.  
var G_WRITE = 16 # Write by groups.  
var G_EXEC  = 8  # Execute by groups. 

var O_READ  = 4  # Read by others.  
var O_WRITE = 2  # Write by others.  
var O_EXEC  = 1  # Execute by others. 

var DEFAULT_PERMESSIONS = U_READ | U_WRITE | G_READ | O_READ # 0644

var SEEK_SET = 0
var SEEK_CUR = 1
var SEEK_END = 2

var BUFFER_SIZE = 104857

class File<Closable>{
    init(path, mode, permissions){
        if (type(path) != "string"){
            print_error("File path must be a string")
        }
        if(!fileExists(path)){
            mode = mode | F_CR
        }
        if(!permissions)
            permissions = DEFAULT_PERMESSIONS
        this.fd = open(path, mode, permissions)
        this.mode = mode
        this.path = path
        this.size = fileSize(path)
        this.cursor = 0
    }
    _size_(){
        return this.size
    }

    readAll(){
        if (this.mode & F_WO){
            print_error("Cannot read from write-only file")
        }
        if(this.size > BUFFER_SIZE){
            print_error("File is too large to read all at once. Size: ${} > MAX: ${1}\nUse read(size: int) and buffer the file instead.".f(this.size, BUFFER_SIZE))
        }
        seek(this.fd, 0, SEEK_SET)
        var ret = read(this.fd)
        this.cursor = this.size
        return ret
    }

    read(size){
        if (this.mode & F_WO){
            print_error("Cannot read from write-only file")
        }
        seek(this.fd, this.cursor, SEEK_SET)
        var ret = readN(this.fd, size)
        this.cursor = this.cursor + len(ret)
        return ret
    }

    append(data){
        if (this.mode & F_RO){
            print_error("Cannot write to read-only file")
        }
        seek(this.fd, 0, SEEK_END)
        write(this.fd, data)
        this.size = this.size + len(data)
        this.cursor = this.size
        return data
    }

    write(data){
        if (this.mode & F_RO){
            print_error("Cannot write to read-only file")
        }
        seek(this.fd, this.cursor, SEEK_SET)
        write(this.fd, data)
        this.cursor = this.cursor + len(data)
        if (this.cursor > this.size){
            this.size = this.cursor
        }   
        return data
    }
    resetCursor(){
        this.cursor = 0
    }
    end(){
        seek(this.fd, 0, SEEK_END)
        this.cursor = this.size
    }
    isAtEnd(){
        return this.cursor == this.size
    }
    
    seekFromStart(offest){
        seek(this.fd, offest, SEEK_SET)
        this.cursor = offest
    }
    seekFromCurrent(offest){
        seek(this.fd, offest, SEEK_CUR)
        this.cursor = this.cursor + offest
    }
    seekFromEnd(offest){
        seek(this.fd, -offest, SEEK_END)
        this.cursor = this.size - offest
    }
    close(){
        if (close(this.fd))
            this.fd = -1
        else
            print_error("Failed to close file")
        return true
    }

    #File Operations:
# openFile(path: string, mode: string): fileDescriptor: Opens a file and returns a file descriptor.
# closeFile(fd: fileDescriptor): void: Closes a file given its file descriptor.
# Reading from Files:

# TODO
# readFile(fd: fileDescriptor, size: int): string: Reads a specified number of bytes from a file.
# readLine(fd: fileDescriptor): string: Reads a line from a file.


# Writing to Files:

# writeFile(fd: fileDescriptor, data: string): void: Writes data to a file.
# appendFile(fd: fileDescriptor, data: string): void: Appends data to the end of a file.
# File Navigation:

# seekFile(fd: fileDescriptor, offset: int, whence: int): void: Moves the file pointer to a specified location.
}