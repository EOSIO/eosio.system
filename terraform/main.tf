provider "aws" {
    region = "us-west-2"
}

terraform {
  backend "s3" {
    bucket = "eosio-system-tf-state"
    key    = "eosio.system.tfstate"
    region = "us-west-2"
  }
}

provider "buildkite" {
  api_token    = "${data.aws_ssm_parameter.buildkite_api_key.value}"
  organization = "EOSIO"
  version = "0.0.3"
}
