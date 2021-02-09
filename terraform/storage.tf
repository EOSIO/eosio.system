resource "aws_s3_bucket" "eosio_system_tf_state" {
  bucket = "eosio-system-tf-state"
  region = "us-west-2"
  lifecycle {
    prevent_destroy = true
  }
  tags = {
    billing-use = "ci-eosio",
    Name = "Store terraform state for eosio.system",
    pipeline = "eosio.system",
    repo = "eosio.system"
  }
}
