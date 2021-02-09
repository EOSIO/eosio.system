data "aws_ssm_parameter" "buildkite_api_key" {
  name  = "/terraform/buildkite-api-key-fullaccess"
}
