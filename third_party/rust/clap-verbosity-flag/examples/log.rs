use clap::Parser;
use clap_verbosity_flag::Verbosity;

/// Foo
#[derive(Debug, Parser)]
struct Cli {
    #[command(flatten)]
    verbosity: Verbosity,
}

fn main() {
    let cli = Cli::parse();

    env_logger::Builder::new()
        .filter_level(cli.verbosity.into())
        .init();

    log::error!("Engines exploded");
    log::warn!("Engines smoking");
    log::info!("Engines exist");
    log::debug!("Engine temperature is 200 degrees");
    log::trace!("Engine subsection is 300 degrees");
}
