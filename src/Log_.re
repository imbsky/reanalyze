let useOcamlLocations = true;

let posIsReason = (pos: Lexing.position) =>
  Filename.check_suffix(pos.pos_fname, ".re")
  || Filename.check_suffix(pos.pos_fname, ".rei");

module Color = {
  let color_enabled = lazy(Unix.isatty(Unix.stdout));
  let forceColor = ref(false);

  let get_color_enabled = () => {
    forceColor^ || Lazy.force(color_enabled);
  };

  type color =
    | Red
    | Yellow
    | Magenta
    | Cyan;

  type style =
    | FG(color)
    | Bold
    | Dim;

  let code_of_style =
    fun
    | FG(Red) => "31"
    | FG(Yellow) => "33"
    | FG(Magenta) => "35"
    | FG(Cyan) => "36"

    | Bold => "1"
    | Dim => "2";

  let style_of_tag = s =>
    switch (s |> Compat.getStringTag) {
    | "error" => [Bold, FG(Red)]
    | "warning" => [Bold, FG(Magenta)]
    | "info" => [Bold, FG(Yellow)]
    | "dim" => [Dim]
    | "filename" => [FG(Cyan)]
    | _ => []
    };
  let ansi_of_tag = s => {
    let l = style_of_tag(s);
    let s = String.concat(";", List.map(code_of_style, l));
    "\027[" ++ s ++ "m";
  };

  let reset_lit = "\027[0m";

  let color_functions =
    Compat.setOpenCloseTag(
      s =>
        if (get_color_enabled()) {
          ansi_of_tag(s);
        } else {
          "";
        },
      _ =>
        if (get_color_enabled()) {
          reset_lit;
        } else {
          "";
        },
    );

  let setup = () => {
    Format.pp_set_mark_tags(Format.std_formatter, true);
    Compat.pp_set_formatter_tag_functions(
      Format.std_formatter,
      color_functions,
    );
    if (!get_color_enabled()) {
      Misc.Color.setup(Some(Never));
    };
    if (useOcamlLocations) {
      // do on extra dummy print, as the first time print_loc is used, flushing is incorrect
      Location.print_loc(
        Format.str_formatter,
        Location.none,
      );
    };
  };

  let error = (ppf, s) => Format.fprintf(ppf, "@{<error>%s@}", s);
  let info = (ppf, s) => Format.fprintf(ppf, "@{<info>%s@}", s);
};

module Loc = {
  let print_filename = (ppf, file) =>
    switch (file) {
    /* modified */
    | "_none_"
    | "" => Format.fprintf(ppf, "(No file name)")
    | real_file =>
      Format.fprintf(ppf, "%s", Location.show_filename(real_file))
    };

  let print_loc = (~normalizedRange, ppf, loc: Location.t) => {
    let (file, _, _) = Location.get_pos_info(loc.loc_start);
    if (useOcamlLocations) {
      let mkPosRelative = (pos: Lexing.position) => {
        ...pos,
        pos_fname:
          Filename.(
            is_implicit(pos.pos_fname)
              ? concat(current_dir_name, pos.pos_fname) : pos.pos_fname
          ),
      };
      Location.print_loc(
        ppf,
        {
          ...loc,
          loc_start: loc.loc_start |> mkPosRelative,
          loc_end: loc.loc_end |> mkPosRelative,
        },
      );
    } else {
      let dim_loc = ppf =>
        fun
        | None => ()
        | Some((
            (start_line, start_line_start_char),
            (end_line, end_line_end_char),
          )) =>
          if (start_line == end_line) {
            if (start_line_start_char == end_line_end_char) {
              Format.fprintf(
                ppf,
                " @{<dim>%i:%i@}",
                start_line,
                start_line_start_char,
              );
            } else {
              Format.fprintf(
                ppf,
                " @{<dim>%i:%i-%i@}",
                start_line,
                start_line_start_char,
                end_line_end_char,
              );
            };
          } else {
            Format.fprintf(
              ppf,
              " @{<dim>%i:%i-%i:%i@}",
              start_line,
              start_line_start_char,
              end_line,
              end_line_end_char,
            );
          };

      Format.fprintf(
        ppf,
        "File \"%a\", line %a",
        print_filename,
        file,
        dim_loc,
        normalizedRange,
      );
    };
  };

  let print = (ppf, loc: Location.t) => {
    let (_file, start_line, start_char) =
      Location.get_pos_info(loc.loc_start);
    let (_, end_line, end_char) = Location.get_pos_info(loc.loc_end);
    let normalizedRange =
      if (start_char === (-1) || end_char === (-1)) {
        None;
      } else if (start_line == end_line && start_char >= end_char) {
        let same_char = start_char + 1;
        Some(((start_line, same_char), (end_line, same_char)));
      } else {
        Some(((start_line, start_char + 1), (end_line, end_char)));
      };
    Format.fprintf(ppf, "@[%a@]", print_loc(~normalizedRange), loc);
  };
};

let log = x => {
  Format.fprintf(Format.std_formatter, x);
};

let item = x => {
  Format.fprintf(Format.std_formatter, "  ");
  Format.fprintf(Format.std_formatter, x);
};

let logKind = (body, ~color, ~loc, ~name) => {
  Format.fprintf(
    Format.std_formatter,
    "@[<v 2>@,%a@,%a@,%a@]@.",
    color,
    name,
    Loc.print,
    loc,
    body,
    (),
  );
};

let info = (body, ~loc, ~name) =>
  logKind(body, ~color=Color.info, ~loc, ~name);
let error = (body, ~loc, ~name) =>
  logKind(body, ~color=Color.error, ~loc, ~name);