variable "TAG" {
  default = "latest"
}

variable "BUILD_GIT_HASH" {
  default = ""
}

variable "BUILD_TIMESTAMP" {
  default = ""
}

group "default" {
  targets = ["musl", "glibc"]
}

target "base" {
  context = "."
  target  = "runtime"
  args = {
    BUILD_GIT_HASH  = BUILD_GIT_HASH
    BUILD_TIMESTAMP = BUILD_TIMESTAMP
  }
}

target "musl" {
  inherits   = ["base"]
  dockerfile = "Dockerfile.musl"
  tags       = ["themackabu/ant:${TAG}"]
}

target "glibc" {
  inherits   = ["base"]
  dockerfile = "Dockerfile.glibc"
  tags       = ["themackabu/ant:${TAG}-glibc"]
}
