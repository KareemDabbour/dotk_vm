var theta_spacing = 0.07
var phi_spacing = 0.02

var R1 = 1
var R2 = 2
var K2 = 5
var A = 0
var B = 0
var screenWidth = 20
var screenHeight = 20
var K1 = screenHeight*K2*3/(8*(R1+R2))

for(;;){  
  var cosA = cos(A)
  var sinA = sin(A)
  var cosB = cos(B)
  var sinB = sin(B)
  var output = [screenHeight].map(fn(x)=>[screenWidth]{" "})
  var zbuffer = [screenHeight].map(fn(x)=>[screenWidth]{0})

  for (var theta = 0; theta < 6.28; theta += theta_spacing){
    var costheta = cos(theta)
    var sintheta = sin(theta)
    for(var phi = 0; phi < 6.28; phi += phi_spacing){
      var cosphi = cos(phi)
      var sinphi = sin(phi)
      var circlex = R2 + R1 * costheta
      var circley = sintheta
      var circlez = R1 * sintheta
      var x = circlex * (cosB * cosphi + sinA * sinB * sinphi) - circley * cosA * sinB
      var y = circlex * (sinB * cosphi - sinA * cosB * sinphi) + circley * cosA * cosB
      var z = K2 + cosA * circlex * sinphi + circley * sinA
      var ooz = 1 / z
      var xp = int(screenHeight/2 + K1* ooz * x)
      var yp = int(screenWidth/2 - K1* ooz * y)
      var L = cosphi * costheta * sinB - cosA * costheta * sinphi - sinA * sintheta + cosB * (cosA * sintheta - costheta * sinA * sinphi)
      if (L > 0){
        if (ooz > zbuffer[xp][yp]){
          zbuffer[xp][yp] = ooz
          var luminance_index = (L * 8) // 1
          output[xp][yp] = ".,-~:;=!*#$@"[luminance_index]
        }
      }
    }
  }
  clear()
  for (var i = 0; i < screenHeight; i+=1){
    write(1, "".join(output[i]))
    write(1, "\n")
  }
  A += 0.04
  B += 0.02
}