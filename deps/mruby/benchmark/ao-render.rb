# AO render benchmark
# Original program (C) Syoyo Fujita in Javascript (and other languages)
#      http://lucille.atso-net.jp/blog/?p=642
#      http://lucille.atso-net.jp/blog/?p=711
# Ruby(yarv2llvm) version by Hideki Miura
# mruby version by Hideki Miura
#

IMAGE_WIDTH = 256
IMAGE_HEIGHT = 256
NSUBSAMPLES = 2
NAO_SAMPLES = 8

module Rand
  # Use xorshift
  @@x = 123456789
  @@y = 362436069
  @@z = 521288629
  @@w = 88675123
  BNUM = 1 << 29
  BNUMF = BNUM.to_f
  def self.rand
    x = @@x
    t = x ^ ((x & 0xfffff) << 11)
    w = @@w
    @@x, @@y, @@z = @@y, @@z, w
    w = @@w = (w ^ (w >> 19) ^ (t ^ (t >> 8)))
    (w % BNUM) / BNUMF
  end
end

class Vec
  def initialize(x, y, z)
    @x = x
    @y = y
    @z = z
  end

  def x=(v); @x = v; end
  def y=(v); @y = v; end
  def z=(v); @z = v; end
  def x; @x; end
  def y; @y; end
  def z; @z; end

  def vadd(b)
    Vec.new(@x + b.x, @y + b.y, @z + b.z)
  end

  def vsub(b)
    Vec.new(@x - b.x, @y - b.y, @z - b.z)
  end

  def vcross(b)
    Vec.new(@y * b.z - @z * b.y,
            @z * b.x - @x * b.z,
            @x * b.y - @y * b.x)
  end

  def vdot(b)
    r = @x * b.x + @y * b.y + @z * b.z
    r
  end

  def vlength
    Math.sqrt(@x * @x + @y * @y + @z * @z)
  end

  def vnormalize
    len = vlength
    v = Vec.new(@x, @y, @z)
    if len > 1.0e-17 then
      v.x = v.x / len
      v.y = v.y / len
      v.z = v.z / len
    end
    v
  end
end


class Sphere
  def initialize(center, radius)
    @center = center
    @radius = radius
  end

  def center; @center; end
  def radius; @radius; end

  def intersect(ray, isect)
    rs = ray.org.vsub(@center)
    b = rs.vdot(ray.dir)
    c = rs.vdot(rs) - (@radius * @radius)
    d = b * b - c
    if d > 0.0 then
      t = - b - Math.sqrt(d)

      if t > 0.0 and t < isect.t then
        isect.t = t
        isect.hit = true
        isect.pl = Vec.new(ray.org.x + ray.dir.x * t,
                          ray.org.y + ray.dir.y * t,
                          ray.org.z + ray.dir.z * t)
        n = isect.pl.vsub(@center)
        isect.n = n.vnormalize
      end
    end
  end
end

class Plane
  def initialize(p, n)
    @p = p
    @n = n
  end

  def intersect(ray, isect)
    d = -@p.vdot(@n)
    v = ray.dir.vdot(@n)
    v0 = v
    if v < 0.0 then
      v0 = -v
    end
    if v0 < 1.0e-17 then
      return
    end

    t = -(ray.org.vdot(@n) + d) / v

    if t > 0.0 and t < isect.t then
      isect.hit = true
      isect.t = t
      isect.n = @n
      isect.pl = Vec.new(ray.org.x + t * ray.dir.x,
                        ray.org.y + t * ray.dir.y,
                        ray.org.z + t * ray.dir.z)
    end
  end
end

class Ray
  def initialize(org, dir)
    @org = org
    @dir = dir
  end

  def org; @org; end
  def org=(v); @org = v; end
  def dir; @dir; end
  def dir=(v); @dir = v; end
end

class Isect
  def initialize
    @t = 10000000.0
    @hit = false
    @pl = Vec.new(0.0, 0.0, 0.0)
    @n = Vec.new(0.0, 0.0, 0.0)
  end

  def t; @t; end
  def t=(v); @t = v; end
  def hit; @hit; end
  def hit=(v); @hit = v; end
  def pl; @pl; end
  def pl=(v); @pl = v; end
  def n; @n; end
  def n=(v); @n = v; end
end

def clamp(f)
  i = f * 255.5
  if i > 255.0 then
    i = 255.0
  end
  if i < 0.0 then
    i = 0.0
  end
  i.to_i
end

def otherBasis(basis, n)
  basis[2] = Vec.new(n.x, n.y, n.z)
  basis[1] = Vec.new(0.0, 0.0, 0.0)

  if n.x < 0.6 and n.x > -0.6 then
    basis[1].x = 1.0
  elsif n.y < 0.6 and n.y > -0.6 then
    basis[1].y = 1.0
  elsif n.z < 0.6 and n.z > -0.6 then
    basis[1].z = 1.0
  else
    basis[1].x = 1.0
  end

  basis[0] = basis[1].vcross(basis[2])
  basis[0] = basis[0].vnormalize

  basis[1] = basis[2].vcross(basis[0])
  basis[1] = basis[1].vnormalize
end

class Scene
  def initialize
    @spheres = Array.new
    @spheres[0] = Sphere.new(Vec.new(-2.0, 0.0, -3.5), 0.5)
    @spheres[1] = Sphere.new(Vec.new(-0.5, 0.0, -3.0), 0.5)
    @spheres[2] = Sphere.new(Vec.new(1.0, 0.0, -2.2), 0.5)
    @plane = Plane.new(Vec.new(0.0, -0.5, 0.0), Vec.new(0.0, 1.0, 0.0))
  end

  def ambient_occlusion(isect)
    basis = Array.new(3)
    otherBasis(basis, isect.n)

    ntheta    = NAO_SAMPLES
    nphi      = NAO_SAMPLES
    eps       = 0.0001
    occlusion = 0.0

    p0 = Vec.new(isect.pl.x + eps * isect.n.x,
                isect.pl.y + eps * isect.n.y,
                isect.pl.z + eps * isect.n.z)
    nphi.times do |j|
      ntheta.times do |i|
        r = Rand::rand
        phi = 2.0 * 3.14159265 * Rand::rand
        x = Math.cos(phi) * Math.sqrt(1.0 - r)
        y = Math.sin(phi) * Math.sqrt(1.0 - r)
        z = Math.sqrt(r)

        rx = x * basis[0].x + y * basis[1].x + z * basis[2].x
        ry = x * basis[0].y + y * basis[1].y + z * basis[2].y
        rz = x * basis[0].z + y * basis[1].z + z * basis[2].z

        raydir = Vec.new(rx, ry, rz)
        ray = Ray.new(p0, raydir)

        occisect = Isect.new
        @spheres[0].intersect(ray, occisect)
        @spheres[1].intersect(ray, occisect)
        @spheres[2].intersect(ray, occisect)
        @plane.intersect(ray, occisect)
        if occisect.hit then
          occlusion = occlusion + 1.0
        else
          0.0
        end
      end
    end

    occlusion = (ntheta.to_f * nphi.to_f - occlusion) / (ntheta.to_f * nphi.to_f)
    Vec.new(occlusion, occlusion, occlusion)
  end

  def render(w, h, nsubsamples)
    cnt = 0
    nsf = nsubsamples.to_f
    h.times do |y|
      w.times do |x|
        rad = Vec.new(0.0, 0.0, 0.0)

        # Subsmpling
        nsubsamples.times do |v|
          nsubsamples.times do |u|
            cnt = cnt + 1
            wf = w.to_f
            hf = h.to_f
            xf = x.to_f
            yf = y.to_f
            uf = u.to_f
            vf = v.to_f

            px = (xf + (uf / nsf) - (wf / 2.0)) / (wf / 2.0)
            py = -(yf + (vf / nsf) - (hf / 2.0)) / (hf / 2.0)

            eye = Vec.new(px, py, -1.0).vnormalize

            ray = Ray.new(Vec.new(0.0, 0.0, 0.0), eye)

            isect = Isect.new
            @spheres[0].intersect(ray, isect)
            @spheres[1].intersect(ray, isect)
            @spheres[2].intersect(ray, isect)
            @plane.intersect(ray, isect)
            if isect.hit then
              col = ambient_occlusion(isect)
              rad.x = rad.x + col.x
              rad.y = rad.y + col.y
              rad.z = rad.z + col.z
            else
              0.0
            end
          end
        end

        r = rad.x / (nsf * nsf)
        g = rad.y / (nsf * nsf)
        b = rad.z / (nsf * nsf)
        printf("%c", clamp(r))
        printf("%c", clamp(g))
        printf("%c", clamp(b))
      end
    end
  end
end

# File.open("ao.ppm", "w") do |fp|
  printf("P6\n")
  printf("%d %d\n", IMAGE_WIDTH, IMAGE_HEIGHT)
  printf("255\n", IMAGE_WIDTH, IMAGE_HEIGHT)
  Scene.new.render(IMAGE_WIDTH, IMAGE_HEIGHT, NSUBSAMPLES)
#  Scene.new.render(256, 256, 2)
# end
