require 'open3'
require 'socket'
require 'net/http'
require 'json'
require 'timeout'
require 'pty'

BIN = File.expand_path(ARGV.fetch(0))
EXAMPLES = File.expand_path('../examples', __dir__)

def with_server(example, limit, files_root: nil)
  arguments = [BIN, '--net']
  arguments += ['--files-root', files_root] if files_root
  arguments += [File.join(EXAMPLES, example), 'serve', '0', limit.to_s]
  Open3.popen3(*arguments) do |input, output, errors, process|
    input.close
    begin
      marker = Timeout.timeout(5) { output.gets }
      raise "Server startup failed: #{marker.inspect}" unless marker && marker.include?('listening')
      port = marker.match(/:(\d+)/)[1].to_i
      yield port
      remaining = output.read
      diagnostics = errors.read
      result = process.value
      raise "Server failed: #{remaining} #{diagnostics}" unless result.success? && diagnostics.empty?
    ensure
      Process.kill('TERM', process.pid) if process.alive?
      process.value
    end
  end
end

def command(*arguments)
  output, errors, result = Open3.capture3(BIN, *arguments)
  raise "Command failed: #{output} #{errors}" unless result.success? && errors.empty?
  output
end

def http_request(port, method, path, body = nil)
  client = Net::HTTP.new('127.0.0.1', port, nil)
  client.open_timeout = 3
  client.read_timeout = 3
  request = method == 'POST' ? Net::HTTP::Post.new(path) : Net::HTTP::Get.new(path)
  request['Connection'] = 'close'
  request.body = body if body
  client.request(request)
end

def rejected_request(port, bytes, eof = false)
  TCPSocket.open('127.0.0.1', port) do |socket|
    socket.write(bytes)
    socket.close_write if eof
    begin
      response = socket.read
      raise 'Malformed request was accepted' unless response.empty?
    rescue Errno::ECONNRESET
    end
  end
end

def until_marker(reader, marker)
  output = ''.b
  output << reader.readpartial(4096) until output.include?(marker.b)
  output
end

Timeout.timeout(45) do
  lifecycle = command('--net', '-e', "let listener = Sockets.listen('127.0.0.1', 0); let empty = Sockets.accept(listener, 0); let client = Sockets.connect('127.0.0.1', Sockets.port(listener), 1000); let peer = Sockets.accept(listener, 1000); System.print(empty == null, Sockets.receive(client, 1, 0) == null); Sockets.send(peer, b'A\\x00B', 1000); let data = Sockets.receive(client, 8, 1000); System.print(data == b'A\\x00B'); Sockets.close(peer); System.print(Sockets.receive(client, 8, 1000) == b''); Sockets.close(client); Sockets.close(listener)")
  raise 'Socket timeout/bytes/EOF lifecycle failed' unless lifecycle == "true true\ntrue\ntrue\n"
  raise 'Combined network/files capabilities failed' unless command('--net', '--files-root', EXAMPLES, '-e', "System.print(Files.exists('tetris.k3')); let server = Sockets.listen('127.0.0.1', 0); Sockets.close(server)") == "true\n"
  denied = command('-e', "try { Sockets.listen('127.0.0.1', 0) } catch error { System.print(error.kind == Error.Kind.Capability) }")
  raise 'Network was enabled by default' unless denied == "true\n"
  numeric = command('--net', '-e', "try { Sockets.listen('localhost', 0) } catch error { System.print(error.kind == Error.Kind.IO) }")
  raise 'Numeric-address policy failed' unless numeric == "true\n"

  with_server('webserver.k3', 16) do |port|
    root = http_request(port, 'GET', '/')
    raise 'Root route failed' unless root.code == '200' && root.body == "DotK 3 web server\n"
    health = http_request(port, 'GET', '/health')
    raise 'Health JSON route failed' unless health.code == '200' && JSON.parse(health.body) == { 'ok' => true, 'language' => 'DotK 3' }
    payload = ("A\x00\xffB".b * 131072)
    echo = http_request(port, 'POST', '/echo', payload)
    raise 'Large binary HTTP echo failed' unless echo.code == '200' && echo.body.b == payload
    native = command('--net', File.join(EXAMPLES, 'http_client.k3'), '127.0.0.1', port.to_s, '/health')
    raise 'DotK HTTP client failed' unless native.start_with?("200\n") && JSON.parse(native.lines[1])['ok']
    raise 'Missing route failed' unless http_request(port, 'GET', '/missing').code == '404'
    requests = command('--net', '-e', "let health = Requests.get('http://127.0.0.1:#{port}/health'); let echo = Requests.post('http://127.0.0.1:#{port}/echo', {'body': 'ping', 'headers': {'X-Test': 'yes'}}); let missing = Requests.get('http://127.0.0.1:#{port}/missing'); System.print(health['status'], health['ok'], Requests.json(health)['language'], echo['body'], missing['status'], missing['ok'])")
    raise 'Requests module failed' unless requests == "200 true DotK 3 ping 404 false\n"
    refused = command('--net', '-e', "try { Requests.get('http://127.0.0.1:1/', {'timeoutMs': 500}) } catch error { System.print(error.kind == Error.Kind.IO) }")
    raise 'Requests connection failure should be an IO error' unless refused == "true\n"
    TCPSocket.open('127.0.0.1', port) do |socket|
      socket.write("POST /echo HTTP/1.1\r")
      socket.write("\nHost: local\r\nContent-Length: 4\r\n\r\n")
      socket.write("\xc3".b)
      socket.write("\xa9\x00Z".b)
      response = socket.read.b
      header, body = response.split("\r\n\r\n".b, 2)
      raise 'Split UTF-8/binary byte length failed' unless header.include?('Content-Length: 4') && body == "\xc3\xa9\x00Z".b
    end
    rejected_request(port, "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n")
    rejected_request(port, "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
    rejected_request(port, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nno", true)
    rejected_request(port, "GET / HTTP/1.1\r\nHost: x\r\n")
    rejected_request(port, "GET / HTTP/1.1\r\nHost: " + 'x' * 17000 + "\r\n\r\n")
    rejected_request(port, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n")
    raise 'Server was poisoned by rejected oversized input' unless http_request(port, 'GET', '/health').code == '200'
  end

  with_server('browser_server.k3', 14, files_root: File.join(EXAMPLES, 'browser_ui')) do |port|
    root = http_request(port, 'GET', '/')
    raise 'Browser HTML failed' unless root.code == '200' && root.body.include?('<canvas') && root['content-type'].start_with?('text/html')
    %w[/app.css /app.js /icons.js].each do |path|
      asset = http_request(port, 'GET', path)
      raise "Static browser asset failed: #{path}" unless asset.code == '200' && asset.body.length > 100
    end
    font = http_request(port, 'GET', '/arcade.woff2')
    raise 'Local font failed' unless font.code == '200' && font.body.b.start_with?('wOF2') && font['content-type'] == 'font/woff2'
    initial = JSON.parse(http_request(port, 'GET', '/api/state').body)
    raise 'Initial authoritative game state failed' unless initial['status'] == 'ready' && initial['board'].length == 868 && initial['ghosts'].length == 3
    started = JSON.parse(http_request(port, 'POST', '/api/action', JSON.generate(action: 'right')).body)
    raise 'Game start action failed' unless started['status'] == 'playing'
    paused = JSON.parse(http_request(port, 'POST', '/api/action', JSON.generate(action: 'pause')).body)
    raise 'Game pause failed' unless paused['status'] == 'paused'
    stable = JSON.parse(http_request(port, 'GET', '/api/state').body)
    raise 'Paused game advanced' unless stable['ticks'] == paused['ticks'] && stable['player'] == paused['player']
    raise 'Unknown action did not return 400' unless http_request(port, 'POST', '/api/action', JSON.generate(action: 'bad')).code == '400'
    raise 'Invalid JSON did not return 400' unless http_request(port, 'POST', '/api/action', '{').code == '400'
    raise 'UI route traversal allowed' unless http_request(port, 'GET', '/../app.css').code == '404'
    reset = JSON.parse(http_request(port, 'POST', '/api/action', JSON.generate(action: 'restart')).body)
    raise 'Restart failed' unless reset['status'] == 'ready' && reset['score'] == 0 && reset['lives'] == 3
    raise 'UI server poisoned by invalid request' unless http_request(port, 'GET', '/api/state').code == '200'
  end

  with_server('chat_server.k3', 2) do |port|
    TCPSocket.open('127.0.0.1', port) do |peer|
      raise 'Chat welcome failed' unless peer.gets == "Welcome to DotK 3 chat\n"
      peer.write("/nick Ada\n")
      TCPSocket.open('127.0.0.1', port) { |disconnected| disconnected.gets }
      TCPSocket.open('127.0.0.1', port) do |oversized|
        oversized.gets
        oversized.write('x' * 2049 + "\n")
        raise 'Oversized chat message was broadcast' unless oversized.read.empty?
      end
      native = command('--net', File.join(EXAMPLES, 'chat_client.k3'), 'send', '127.0.0.1', port.to_s, 'hello')
      raise 'DotK chat client failed' unless native.include?("cli: hello\n")
      raise 'Multi-client broadcast failed' unless peer.gets == "cli: hello\n"
      peer.write("caf\xc3".b)
      peer.write("\xa9\n".b)
      raise 'Split UTF-8 chat failed' unless peer.gets.b == "Ada: caf\xc3\xa9\n".b
      raise 'Chat graceful shutdown failed' unless peer.read.include?('Server closing')
    end
  end

  with_server('chat_server.k3', 2) do |port|
    PTY.spawn({ 'TERM' => 'xterm' }, BIN, '--net', '--steps', '0', File.join(EXAMPLES, 'chat_client.k3'), 'connect', '127.0.0.1', port.to_s) do |reader, writer, process|
      until_marker(reader, 'Welcome to DotK 3 chat')
      writer.write("/nick Player\nhellx\x7fo\n"); writer.flush
      until_marker(reader, 'Player: hello')
      writer.write("\x03"); writer.flush
      output = ''.b
      begin
        loop { output << reader.readpartial(4096) }
      rescue EOFError, Errno::EIO
      end
      Process.wait(process)
      raise 'Interactive chat controls/exit failed' unless output.include?('Disconnected') && $?.success?
    end
    TCPSocket.open('127.0.0.1', port) do |peer|
      peer.gets
      peer.write("last\n")
      raise 'Chat survived interactive disconnect incorrectly' unless peer.read.include?('guest: last')
    end
  end
end
puts 'network: PASS (socket lifecycle, web/binary/malformed HTTP, multi-client chat, interactive chat PTY)'