resource "buildkite_pipeline" "eosio.system" {
  name                 = "eosio.system"
  repository           = "git@github.com:EOSIO/eosio.system.git"
  slug                 = "eosio.system"
  default_branch       = "master"
  description          = "Linters and deployments for the eosio.system repository"

  step = [
    {
      type    = "script"
      name    = ":pipeline: Pipeline Upload"
      command = ".buildkite/pipeline-upload.sh"
      agent_query_rules = [
        "queue=automation-basic-builder-fleet"
      ]
    },
  ]
}
output "eosio.system" {
  value = "${buildkite_pipeline.eosio.system.webhook_url}"
}
